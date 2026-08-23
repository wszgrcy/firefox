/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.geckoview;

import androidx.annotation.AnyThread;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import org.mozilla.gecko.annotation.WrapForJNI;

/**
 * Static JNI bridge used by native Gecko to query whether an http/https request should be
 * intercepted and served with custom content.
 *
 * <p>The native request-interception code calls {@link #intercept(String, String, String[])} for
 * every http/https channel. If a {@link WebResponse} is returned, its {@link WebResponse#body}
 * stream is served as the response body while the original URI is preserved.
 *
 * <p>This class is package private: it is only used internally, via the public {@link
 * GeckoSession.RequestInterceptor} API.
 */
/* package */ final class GeckoViewRequestInterceptor {
  private static @Nullable GeckoSession.RequestInterceptor sInterceptor;

  private GeckoViewRequestInterceptor() {}

  /** Set the {@link GeckoSession.RequestInterceptor} queried by native code. */
  @AnyThread
  public static void setInterceptor(
      final @Nullable GeckoSession.RequestInterceptor interceptor) {
    sInterceptor = interceptor;
  }

  /** Return the currently registered {@link GeckoSession.RequestInterceptor}, if any. */
  @AnyThread
  public static @Nullable GeckoSession.RequestInterceptor getInterceptor() {
    return sInterceptor;
  }

  /**
   * Called by native Gecko for every http/https channel. Returns a {@link WebResponse} if the
   * request should be intercepted, or {@code null} to let Gecko handle the request normally.
   *
   * @param uri the request URI (unchanged)
   * @param method the HTTP method, e.g. "GET"
   * @param headers the request headers as key/value pairs, or {@code null}
   */
  @WrapForJNI
  @AnyThread
  public static @Nullable WebResponse intercept(
      final @NonNull String uri,
      final @NonNull String method,
      final @Nullable String[] headers) {
    final GeckoSession.RequestInterceptor interceptor = sInterceptor;
    if (interceptor == null) {
      return null;
    }
    return interceptor.interceptRequest(uri, method, headers);
  }
}
