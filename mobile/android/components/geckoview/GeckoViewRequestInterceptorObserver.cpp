/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GeckoViewRequestInterceptorObserver.h"

#include <android/log.h>
#include "GeckoViewRequestInterceptorController.h"
#include "nsXULAppAPI.h"
#include "mozilla/jni/Utils.h"
#include "mozilla/java/GeckoViewRequestInterceptorWrappers.h"
#include "mozilla/java/WebResponseWrappers.h"
#include "nsIChannel.h"
#include "nsIChannelEventSink.h"
#include "nsIHttpChannel.h"
#include "nsIHttpChannelInternal.h"
#include "nsIHttpHeaderVisitor.h"
#include "nsILoadInfo.h"
#include "nsIPrincipal.h"
#include "nsIObserverService.h"
#include "nsContentUtils.h"
#include "nsNetUtil.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "mozilla/StaticPrefs_privacy.h"
#include "mozilla/dom/ServiceWorkerDescriptor.h"
#include "mozilla/dom/ServiceWorkerInfo.h"
#include "mozilla/dom/ServiceWorkerManager.h"
#include "mozilla/dom/ServiceWorkerRegistrationInfo.h"
#include "mozilla/StoragePrincipalHelper.h"

using namespace mozilla;

namespace {

// Collects request headers into a jni ObjectArray of "key: value" strings.
class HeaderCollector final : public nsIHttpHeaderVisitor {
 public:
  explicit HeaderCollector(mozilla::jni::ObjectArray::LocalRef aArray,
                           size_t aIndex)
      : mArray(aArray), mIndex(aIndex) {}

  NS_DECL_ISUPPORTS

  NS_IMETHOD VisitHeader(const nsACString& aHeader,
                         const nsACString& aValue) override {
    nsAutoString combined;
    combined.Append(NS_ConvertUTF8toUTF16(aHeader));
    combined.AppendLiteral(u": ");
    combined.Append(NS_ConvertUTF8toUTF16(aValue));
    mArray->SetElement(mIndex++, mozilla::jni::StringParam(combined));
    return NS_OK;
  }

 private:
  ~HeaderCollector() = default;

  mozilla::jni::ObjectArray::LocalRef mArray;
  size_t mIndex;
};

NS_IMPL_ISUPPORTS(HeaderCollector, nsIHttpHeaderVisitor)

// First pass: count the request headers so the ObjectArray can be sized.
class HeaderCounter final : public nsIHttpHeaderVisitor {
 public:
  HeaderCounter() = default;

  NS_DECL_ISUPPORTS

  NS_IMETHOD VisitHeader(const nsACString& aHeader,
                         const nsACString& aValue) override {
    mCount++;
    return NS_OK;
  }

 private:
  ~HeaderCounter() = default;

 public:
  uint32_t mCount = 0;
};

NS_IMPL_ISUPPORTS(HeaderCounter, nsIHttpHeaderVisitor)

}  // namespace

NS_IMPL_ISUPPORTS(GeckoViewRequestInterceptorObserver, nsIObserver)

// static
already_AddRefed<GeckoViewRequestInterceptorObserver>
GeckoViewRequestInterceptorObserver::GetSingleton() {
  static RefPtr<GeckoViewRequestInterceptorObserver> sSingleton =
      new GeckoViewRequestInterceptorObserver();
  return RefPtr<GeckoViewRequestInterceptorObserver>(sSingleton).forget();
}

nsresult GeckoViewRequestInterceptorObserver::OnModifyRequest(
    nsIHttpChannel* aChannel) {
  if (!mozilla::jni::IsAvailable() || !XRE_IsParentProcess()) {
    return NS_OK;
  }

  nsCOMPtr<nsIURI> uri;
  nsresult rv = aChannel->GetURI(getter_AddRefs(uri));
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString scheme;
  rv = uri->GetScheme(scheme);
  NS_ENSURE_SUCCESS(rv, rv);

  // Only intercept http/https.
  if (!scheme.EqualsLiteral("http") && !scheme.EqualsLiteral("https")) {
    return NS_OK;
  }

  nsAutoCString spec;
  rv = uri->GetSpec(spec);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString method;
  rv = aChannel->GetRequestMethod(method);
  NS_ENSURE_SUCCESS(rv, rv);

  // Distinguish navigation (top-level document) from subresource requests, so
  // we can tell whether the custom interceptor is serving the page navigation
  // (which prevents the page from ever being controlled by a service worker).
  bool isNav = nsContentUtils::IsNonSubresourceRequest(aChannel);

  // Detect service worker fallback channels. When a service worker's fetch
  // handler does not call respondWith(), Gecko resets the interception
  // (InterceptedHttpChannel::ResetInterception) and creates a NEW channel for
  // the same URI, flagged REDIRECT_INTERNAL. That new channel re-fires
  // http-on-modify-request, so we see it here. Because ShouldIntercept() never
  // re-intercepts internal redirects, this is the reliable signal that the
  // service worker already tried this request and did not handle it, so we fall
  // through to the custom interceptor.
  bool isInternalRedirect = false;
  nsCOMPtr<nsIHttpChannelInternal> hci = do_QueryInterface(aChannel);
  if (hci) {
    uint32_t redirectFlags = 0;
    hci->GetLastRedirectFlags(&redirectFlags);
    isInternalRedirect =
        (redirectFlags & nsIChannelEventSink::REDIRECT_INTERNAL) != 0;
  }
  __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                      "OnModifyRequest nav=%d internalRedirect=%d uri=%.*s "
                      "method=%.*s",
                      (int)isNav, (int)isInternalRedirect, (int)spec.Length(),
                      spec.get(), (int)method.Length(), method.get());

  // Custom interceptor serves the navigation directly and force-controls the
  // page (sets the service worker controller on the navigation loadInfo). This
  // is required because the service worker cannot itself serve a localhost
  // navigation (its network fetch of a synthetic URL fails), and the page must
  // be controlled so that subresources the custom interceptor does NOT serve
  // (e.g. a remote image) are routed to the service worker.
  //
  // Priority order:
  //  * Navigation: always served directly by the custom interceptor, with the
  //    page force-controlled so the service worker handles subresources the
  //    custom interceptor returns null for.
  //  * Controlled subresources: service-worker-first. The request is offered
  //    to the service worker first. If its fetch handler does not call
  //    respondWith(), Gecko resets the interception and re-fires
  //    http-on-modify-request on an internal-redirect channel, which falls
  //    through to the custom interceptor below (the flag is cleared there so
  //    ShouldIntercept() actually intercepts it). If the handler does handle it
  //    (cache-first + fetch()), its internal fetch also arrives here as an
  //    internal redirect and is served by the custom interceptor.
  //  * Uncontrolled subresources: the Java interceptor is consulted first. If
  //    it returns a response the custom interceptor serves it directly and the
  //    service worker never sees it.
  //  * If the Java interceptor returns null the request is not wrapped, so it
  //    goes to the real network (for a remote resource) or the service worker.
  nsCOMPtr<nsILoadInfo> loadInfo;
  rv = aChannel->GetLoadInfo(getter_AddRefs(loadInfo));
  if (isNav && NS_SUCCEEDED(rv) && loadInfo && !isInternalRedirect) {
    // Force-control the navigation: set the active service worker for this
    // scope as the controller on the navigation loadInfo, so the loaded page
    // is controlled and requests the custom interceptor does not serve are
    // routed to the service worker.
    nsCOMPtr<nsIPrincipal> principal;
    nsresult pv = StoragePrincipalHelper::GetPrincipal(
        aChannel,
        StaticPrefs::privacy_partition_serviceWorkers()
            ? StoragePrincipalHelper::eForeignPartitionedPrincipal
            : StoragePrincipalHelper::eRegularPrincipal,
        getter_AddRefs(principal));
    RefPtr<mozilla::dom::ServiceWorkerManager> swm =
        mozilla::dom::ServiceWorkerManager::GetInstance();
    if (NS_SUCCEEDED(pv) && principal && swm) {
      if (swm->ForceControlClient(principal, uri, loadInfo)) {
        __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                            "OnModifyRequest FORCE-CONTROL navigation");
      }
    }
  }

  // Service-worker-first for controlled subresources. When the page is
  // controlled and this is a subresource that is not itself a service-worker
  // fallback (internal redirect), let the service worker handle it first. If
  // its fetch handler is empty (no respondWith()) Gecko resets the
  // interception and re-fires http-on-modify-request on an internal-redirect
  // channel, which falls through to the custom interceptor below. If it does
  // handle the request, its internal fetch also arrives here as an internal
  // redirect and is served by the custom interceptor.
  if (!isNav && !isInternalRedirect && loadInfo &&
      loadInfo->GetController().isSome()) {
    __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                        "OnModifyRequest service-worker-first (controlled subresource)");
    return NS_OK;
  }

  // Build a String[] of "key: value" headers.
  RefPtr<HeaderCounter> counter = new HeaderCounter();
  rv = aChannel->VisitRequestHeaders(counter);
  NS_ENSURE_SUCCESS(rv, rv);

  auto array =
      mozilla::jni::ObjectArray::New<mozilla::jni::String>(counter->mCount);
  RefPtr<HeaderCollector> collector = new HeaderCollector(array, 0);
  rv = aChannel->VisitRequestHeaders(collector);
  NS_ENSURE_SUCCESS(rv, rv);

  mozilla::java::WebResponse::LocalRef response =
      mozilla::java::GeckoViewRequestInterceptor::Intercept(
          mozilla::jni::StringParam(spec),
          mozilla::jni::StringParam(method), array);
  __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                      "OnModifyRequest Intercept returned response=%p nav=%d",
                      response.Get(), (int)isNav);
  if (!response) {
    return NS_OK;
  }

  mozilla::java::GeckoInputStream::LocalRef body = response->Body();
  if (!body) {
    return NS_OK;
  }

  // Build a controller that stores the status, headers and body of the Java
  // WebResponse. When ShouldIntercept() runs later in OnBeforeConnect() it
  // will find this controller through the channel callbacks and redirect the
  // channel to an InterceptedHttpChannel, which synthesizes the response while
  // keeping the original request URI unchanged.
  RefPtr<GeckoViewRequestInterceptorController> controller =
      GeckoViewRequestInterceptorController::FromWebResponse(response);
  if (!controller) {
    return NS_OK;
  }

  // The service worker script channel is created with LOAD_BYPASS_SERVICE_WORKER
  // so the real service-worker machinery skips it. That flag also makes
  // HttpBaseChannel::ShouldIntercept() skip our custom interceptor (it never
  // reaches ShouldPrepareForIntercept), so the request would go to the real
  // network instead of the synthesized response. Clear it so the request is
  // intercepted as intended. This runs during http-on-modify-request, which
  // happens before ShouldIntercept() in OnBeforeConnect().
  nsIChannel* channel = aChannel;  // nsIHttpChannel extends nsIChannel
  if (channel) {
    uint32_t loadFlags = 0;
    channel->GetLoadFlags(&loadFlags);
    channel->SetLoadFlags(loadFlags & ~nsIChannel::LOAD_BYPASS_SERVICE_WORKER);
  }

  // ShouldIntercept() also skips channels flagged REDIRECT_INTERNAL, which is
  // exactly the kind of channel Gecko creates when a service worker falls back
  // (does not call respondWith()) or when its fetch handler performs an
  // internal fetch of a subresource. Those channels re-fire
  // http-on-modify-request and must be served by the custom interceptor here: a
  // service worker cannot serve a localhost resource, and if we left the flag
  // set the request would fall through to the real network and fail. Clear it
  // so ShouldIntercept() intercepts the channel and delivers the synthesized
  // response. This is safe because we only reach this block when the Java
  // interceptor returned a response and the custom controller below shadows
  // the service worker controller, so clearing the flag cannot cause a
  // service-worker re-intercept loop.
  if (hci) {
    uint32_t redirectFlags = 0;
    hci->GetLastRedirectFlags(&redirectFlags);
    hci->SetLastRedirectFlags(
        redirectFlags & ~nsIChannelEventSink::REDIRECT_INTERNAL);
  }

  // Wrap the channel's existing notification callbacks so that the controller
  // is returned for nsINetworkInterceptController, while every other interface
  // is still delegated to the original callbacks.
  nsCOMPtr<nsIInterfaceRequestor> originalCallbacks;
  rv = aChannel->GetNotificationCallbacks(getter_AddRefs(originalCallbacks));
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<GeckoViewRequestInterceptorControllerRequestor> requestor =
      new GeckoViewRequestInterceptorControllerRequestor(originalCallbacks,
                                                         controller);
  aChannel->SetNotificationCallbacks(requestor);
  __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                      "OnModifyRequest SetNotificationCallbacks requestor=%p",
                      requestor.get());

  return NS_OK;
}

NS_IMETHODIMP
GeckoViewRequestInterceptorObserver::Observe(nsISupports* aSubject,
                                            const char* aTopic,
                                            const char16_t* aData) {
  if (strcmp(aTopic, "app-startup") == 0) {
    // Instantiated at startup; register for http-on-modify-request so every
    // http/https channel is offered to the Java interceptor.
    nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
    if (obs) {
      RefPtr<GeckoViewRequestInterceptorObserver> observer = GetSingleton();
      obs->AddObserver(observer, "http-on-modify-request", false);
    }
    return NS_OK;
  }

  if (strcmp(aTopic, "http-on-modify-request") != 0) {
    return NS_OK;
  }
  nsCOMPtr<nsIHttpChannel> channel = do_QueryInterface(aSubject);
  if (!channel) {
    return NS_OK;
  }
  return OnModifyRequest(channel);
}
