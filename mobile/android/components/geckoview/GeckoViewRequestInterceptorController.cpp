/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "GeckoViewRequestInterceptorController.h"

#include <android/log.h>
#include "GeckoViewInputStream.h"
#include "mozilla/java/GeckoViewInputStreamWrappers.h"
#include "mozilla/java/WebMessageWrappers.h"
#include "nsIChannel.h"
#include "nsIInputStream.h"
#include "nsILoadInfo.h"
#include "nsNetUtil.h"

using namespace mozilla;

namespace {

// nsIInputStream that reads the body of an intercepted Java WebResponse,
// bridging through GeckoViewInputStream.
class GeckoViewInterceptInputStream final : public GeckoViewInputStream {
 public:
  explicit GeckoViewInterceptInputStream(
      java::WebResponse::LocalRef aResponse)
      : GeckoViewInputStream(
            java::GeckoViewInputStream::Create(aResponse->Body())) {}

 private:
  virtual ~GeckoViewInterceptInputStream() = default;
};

}  // namespace

NS_IMPL_ISUPPORTS(GeckoViewRequestInterceptorController,
                  nsINetworkInterceptController)

NS_IMPL_ISUPPORTS(GeckoViewRequestInterceptorControllerRequestor,
                  nsIInterfaceRequestor)

already_AddRefed<GeckoViewRequestInterceptorController>
GeckoViewRequestInterceptorController::FromWebResponse(
    java::WebResponse::LocalRef aResponse) {
  java::GeckoInputStream::LocalRef body = aResponse->Body();
  if (!body) {
    return nullptr;
  }

  RefPtr<GeckoViewRequestInterceptorController> controller =
      new GeckoViewRequestInterceptorController();

  controller->mStatus = static_cast<uint16_t>(aResponse->StatusCode());
  controller->mReason = "Intercepted"_ns;

  java::WebMessage::LocalRef message = aResponse.Cast<java::WebMessage>();
  auto keys = message->GetHeaderKeys();
  auto values = message->GetHeaderValues();
  size_t count = keys->Length();
  for (size_t i = 0; i < count; ++i) {
    jni::String::LocalRef key = keys->GetElement(i);
    jni::String::LocalRef value = values->GetElement(i);
    if (!key || !value) {
      continue;
    }
    controller->mHeaderNames.AppendElement(key->ToCString());
    controller->mHeaderValues.AppendElement(value->ToCString());
  }

  RefPtr<GeckoViewInterceptInputStream> stream =
      new GeckoViewInterceptInputStream(aResponse);
  controller->mBody = stream.get();
  __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                      "FromWebResponse built controller=%p bodyStream=%p",
                      controller.get(), stream.get());

  return controller.forget();
}

NS_IMETHODIMP
GeckoViewRequestInterceptorController::ShouldPrepareForIntercept(
    nsIURI* aURI, nsIChannel* aChannel, bool* aShouldIntercept) {
  *aShouldIntercept = true;
  return NS_OK;
}

NS_IMETHODIMP
GeckoViewRequestInterceptorController::ChannelIntercepted(
    nsIInterceptedChannel* aChannel) {
  __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                      "ChannelIntercepted enter, this=%p aChannel=%p status=%u "
                      "headers=%zu body=%p",
                      this, aChannel, (uint32_t)mStatus, mHeaderNames.Length(),
                      mBody.get());

  // The intercepted channel is a trusted, app-provided response.  Mark its
  // load info as synthesized (like a service worker response) so the channel
  // does not run ORB/cross-origin checks that require a non-null loading
  // principal (top-level navigations have none).
  nsCOMPtr<nsIChannel> channel;
  aChannel->GetChannel(getter_AddRefs(channel));
  nsCOMPtr<nsILoadInfo> loadInfo = channel ? channel->LoadInfo() : nullptr;
  if (loadInfo) {
    loadInfo->SynthesizeServiceWorkerTainting(LoadTainting::Basic);
  } else {
    __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                        "ChannelIntercepted WARN no loadInfo");
  }

  aChannel->SynthesizeStatus(mStatus, mReason);
  for (size_t i = 0; i < mHeaderNames.Length(); ++i) {
    aChannel->SynthesizeHeader(mHeaderNames[i], mHeaderValues[i]);
  }
  aChannel->StartSynthesizedResponse(mBody, nullptr, nullptr, ""_ns, false);
  aChannel->FinishSynthesizedResponse();
  __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                      "ChannelIntercepted done");
  return NS_OK;
}

NS_IMETHODIMP
GeckoViewRequestInterceptorControllerRequestor::GetInterface(
    const nsIID& aIID, void** aResult) {
  if (aIID.Equals(NS_GET_IID(nsINetworkInterceptController))) {
    __android_log_print(ANDROID_LOG_DEBUG, "GVRInterceptor",
                        "Requestor GetInterface -> controller=%p",
                        mController.get());
    *aResult = mController;
    NS_ADDREF(mController);
    return NS_OK;
  }
  if (mOriginal) {
    return mOriginal->GetInterface(aIID, aResult);
  }
  return NS_ERROR_NO_INTERFACE;
}
