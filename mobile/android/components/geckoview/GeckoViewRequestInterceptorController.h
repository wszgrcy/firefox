/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GeckoViewRequestInterceptorController_h_
#define GeckoViewRequestInterceptorController_h_

#include "mozilla/java/WebResponseWrappers.h"
#include "nsCOMPtr.h"
#include "nsINetworkInterceptController.h"
#include "nsIInterfaceRequestor.h"
#include "nsTArray.h"

class nsIInputStream;

// Stores the status, headers and body of an intercepted Java WebResponse and
// synthesizes them on an nsIInterceptedChannel while preserving the original
// request URI.
class GeckoViewRequestInterceptorController final
    : public nsINetworkInterceptController {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSINETWORKINTERCEPTCONTROLLER

  // Builds a controller from a Java WebResponse. Returns null if the response
  // carries no body stream.
  static already_AddRefed<GeckoViewRequestInterceptorController>
  FromWebResponse(mozilla::java::WebResponse::LocalRef aResponse);

 private:
  GeckoViewRequestInterceptorController() = default;
  virtual ~GeckoViewRequestInterceptorController() = default;

  uint16_t mStatus = 200;
  nsCString mReason;
  nsTArray<nsCString> mHeaderNames;
  nsTArray<nsCString> mHeaderValues;
  nsCOMPtr<nsIInputStream> mBody;
};

// nsIInterfaceRequestor that exposes the interceptor controller and delegates
// every other interface to the original channel callbacks.
class GeckoViewRequestInterceptorControllerRequestor final
    : public nsIInterfaceRequestor {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIINTERFACEREQUESTOR

  GeckoViewRequestInterceptorControllerRequestor(
      nsIInterfaceRequestor* aOriginal,
      nsINetworkInterceptController* aController)
      : mOriginal(aOriginal), mController(aController) {}

 private:
  virtual ~GeckoViewRequestInterceptorControllerRequestor() = default;

  nsCOMPtr<nsIInterfaceRequestor> mOriginal;
  nsCOMPtr<nsINetworkInterceptController> mController;
};

#endif  // GeckoViewRequestInterceptorController_h_
