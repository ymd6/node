/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>


static int close_cb_called = 0;


static void close_cb(uv_handle_t* handle) {
  ASSERT(handle != NULL);
  close_cb_called++;
}


TEST_IMPL(tcp_bind6_error_addrinuse) {
  struct sockaddr_in6 addr = uv_ip6_addr("::", TEST_PORT);
  uv_tcp_t server1, server2;
  int r;

  uv_init();

  r = uv_tcp_init(&server1);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server1, addr);
  ASSERT(r == 0);

  r = uv_tcp_init(&server2);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server2, addr);
  ASSERT(r == 0);

  r = uv_tcp_listen(&server1, 128, NULL);
  ASSERT(r == 0);
  r = uv_tcp_listen(&server2, 128, NULL);
  ASSERT(r == -1);

  ASSERT(uv_last_error().code == UV_EADDRINUSE);

  uv_close((uv_handle_t*)&server1, close_cb);
  uv_close((uv_handle_t*)&server2, close_cb);

  uv_run();

  ASSERT(close_cb_called == 2);

  return 0;
}


TEST_IMPL(tcp_bind6_error_addrnotavail) {
  struct sockaddr_in6 addr = uv_ip6_addr("4:4:4:4:4:4:4:4", TEST_PORT);
  uv_tcp_t server;
  int r;

  uv_init();

  r = uv_tcp_init(&server);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server, addr);
  ASSERT(r == -1);
  ASSERT(uv_last_error().code == UV_EADDRNOTAVAIL);

  uv_close((uv_handle_t*)&server, close_cb);

  uv_run();

  ASSERT(close_cb_called == 1);

  return 0;
}


TEST_IMPL(tcp_bind6_error_fault) {
  char garbage[] = "blah blah blah blah blah blah blah blah blah blah blah blah";
  struct sockaddr_in6* garbage_addr;
  uv_tcp_t server;
  int r;

  garbage_addr = (struct sockaddr_in6*) &garbage;

  uv_init();

  r = uv_tcp_init(&server);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server, *garbage_addr);
  ASSERT(r == -1);

  ASSERT(uv_last_error().code == UV_EFAULT);

  uv_close((uv_handle_t*)&server, close_cb);

  uv_run();

  ASSERT(close_cb_called == 1);

  return 0;
}

/* Notes: On Linux uv_bind6(server, NULL) will segfault the program.  */

TEST_IMPL(tcp_bind6_error_inval) {
  struct sockaddr_in6 addr1 = uv_ip6_addr("::", TEST_PORT);
  struct sockaddr_in6 addr2 = uv_ip6_addr("::", TEST_PORT_2);
  uv_tcp_t server;
  int r;

  uv_init();

  r = uv_tcp_init(&server);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server, addr1);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server, addr2);
  ASSERT(r == -1);

  ASSERT(uv_last_error().code == UV_EINVAL);

  uv_close((uv_handle_t*)&server, close_cb);

  uv_run();

  ASSERT(close_cb_called == 1);

  return 0;
}


TEST_IMPL(tcp_bind6_localhost_ok) {
  struct sockaddr_in6 addr = uv_ip6_addr("::1", TEST_PORT);

  uv_tcp_t server;
  int r;

  uv_init();

  r = uv_tcp_init(&server);
  ASSERT(r == 0);
  r = uv_tcp_bind6(&server, addr);
  ASSERT(r == 0);

  return 0;
}
