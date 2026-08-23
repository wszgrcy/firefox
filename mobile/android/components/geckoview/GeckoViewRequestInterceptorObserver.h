/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GeckoViewRequestInterceptorObserver_h_
#define GeckoViewRequestInterceptorObserver_h_

#include "nsCOMPtr.h"
#include "nsIObserver.h"

class nsIHttpChannel;

// Observes "http-on-modify-request" so that every http/https channel can be
// offered to the Java RequestInterceptor. If Java returns a WebResponse, the
// original http channel is replaced by GeckoViewInterceptChannel, which serves
// the custom body while keeping the request URI unchanged.
class GeckoViewRequestInterceptorObserver final : public nsIObserver {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOBSERVER

  GeckoViewRequestInterceptorObserver() = default;

  static already_AddRefed<GeckoViewRequestInterceptorObserver> GetSingleton();

 private:
  virtual ~GeckoViewRequestInterceptorObserver() = default;

  nsresult OnModifyRequest(nsIHttpChannel* aChannel);
};

#endif  // GeckoViewRequestInterceptorObserver_h_
