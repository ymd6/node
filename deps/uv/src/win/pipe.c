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

#include <assert.h>
#include <string.h>

#include "uv.h"
#include "../uv-common.h"
#include "internal.h"


/* A zero-size buffer for use by uv_pipe_read */
static char uv_zero_[] = "";


int uv_pipe_init(uv_pipe_t* handle) {
  uv_stream_init((uv_stream_t*)handle);

  handle->type = UV_NAMED_PIPE;
  handle->reqs_pending = 0;
  handle->pending_accepts = NULL;
  handle->name = NULL;
  handle->handle = INVALID_HANDLE_VALUE;

  uv_counters()->pipe_init++;

  return 0;
}


static int uv_set_pipe_handle(uv_pipe_t* handle, HANDLE pipeHandle) {
  DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;

  if (!SetNamedPipeHandleState(pipeHandle, &mode, NULL, NULL)) {
    return -1;
  }

  if (CreateIoCompletionPort(pipeHandle,
                             LOOP->iocp,
                             (ULONG_PTR)handle,
                             0) == NULL) {
    return -1;
  }

  return 0;
}


void uv_pipe_endgame(uv_pipe_t* handle) {
  uv_err_t err;
  int status;

  if (handle->flags & UV_HANDLE_SHUTTING &&
      !(handle->flags & UV_HANDLE_SHUT) &&
      handle->write_reqs_pending == 0) {
    close_pipe(handle, &status, &err);

    if (handle->shutdown_req->cb) {
      if (status == -1) {
        LOOP->last_error = err;
      }
      handle->shutdown_req->cb(handle->shutdown_req, status);
    }
    handle->reqs_pending--;
  }

  if (handle->flags & UV_HANDLE_CLOSING &&
      handle->reqs_pending == 0) {
    assert(!(handle->flags & UV_HANDLE_CLOSED));
    handle->flags |= UV_HANDLE_CLOSED;

    if (handle->close_cb) {
      handle->close_cb((uv_handle_t*)handle);
    }

    uv_unref();
  }
}


/* Creates a pipe server. */
int uv_pipe_bind(uv_pipe_t* handle, const char* name) {
  int i, errno, nameSize;
  uv_pipe_accept_t* req;

  if (handle->flags & UV_HANDLE_BOUND) {
    uv_set_sys_error(WSAEINVAL);
    return -1;
  }

  if (!name) {
    uv_set_sys_error(WSAEINVAL);
    return -1;
  }

  for (i = 0; i < COUNTOF(handle->accept_reqs); i++) {
    req = &handle->accept_reqs[i];
    uv_req_init((uv_req_t*) req);
    req->type = UV_ACCEPT;
    req->data = handle;
    req->pipeHandle = INVALID_HANDLE_VALUE;
    req->next_pending = NULL;
  }

  /* Convert name to UTF16. */
  nameSize = uv_utf8_to_utf16(name, NULL, 0) * sizeof(wchar_t);
  handle->name = (wchar_t*)malloc(nameSize);
  if (!handle->name) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  if (!uv_utf8_to_utf16(name, handle->name, nameSize / sizeof(wchar_t))) {
    uv_set_sys_error(GetLastError());
    return -1;
  }

  /*
   * Attempt to create the first pipe with FILE_FLAG_FIRST_PIPE_INSTANCE.
   * If this fails then there's already a pipe server for the given pipe name.
   */
  handle->accept_reqs[0].pipeHandle = CreateNamedPipeW(handle->name,
                                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                                       PIPE_UNLIMITED_INSTANCES,
                                                       65536,
                                                       65536,
                                                       0,
                                                       NULL);

  if (handle->accept_reqs[0].pipeHandle == INVALID_HANDLE_VALUE) {
    errno = GetLastError();
    if (errno == ERROR_ACCESS_DENIED) {
      uv_set_error(UV_EADDRINUSE, errno);
      handle->error = LOOP->last_error;
      handle->flags |= UV_HANDLE_BIND_ERROR;
    } else if (errno == ERROR_PATH_NOT_FOUND || errno == ERROR_INVALID_NAME) {
      uv_set_error(UV_EACCESS, errno);
    } else {
      uv_set_sys_error(errno);
    }
    goto error;
  }

  if (uv_set_pipe_handle(handle, handle->accept_reqs[0].pipeHandle)) {
    uv_set_sys_error(GetLastError());
    goto error;
  }

  handle->flags |= UV_HANDLE_PIPESERVER;
  handle->flags |= UV_HANDLE_BOUND;

  return 0;

error:
  if (handle->name) {
    free(handle->name);
    handle->name = NULL;
  }

  if (handle->accept_reqs[0].pipeHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle->accept_reqs[0].pipeHandle);
    handle->accept_reqs[0].pipeHandle = INVALID_HANDLE_VALUE;
  }

  return -1;
}


static DWORD WINAPI pipe_connect_thread_proc(void* parameter) {
  HANDLE pipeHandle = INVALID_HANDLE_VALUE;
  int errno;
  uv_pipe_t* handle;
  uv_connect_t* req;

  req = (uv_connect_t*)parameter;
  assert(req);
  handle = (uv_pipe_t*)req->handle;
  assert(handle);

  /* We're here because CreateFile on a pipe returned ERROR_PIPE_BUSY.  We wait for the pipe to become available with WaitNamedPipe. */
  while (WaitNamedPipeW(handle->name, 30000)) {
    /* The pipe is now available, try to connect. */
    pipeHandle = CreateFileW(handle->name,
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            NULL,
                            OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED,
                            NULL);

    if (pipeHandle != INVALID_HANDLE_VALUE) {
      break;
    }
  }

  if (pipeHandle != INVALID_HANDLE_VALUE && !uv_set_pipe_handle(handle, pipeHandle)) {
    handle->handle = pipeHandle;
    req->error = uv_ok_;
  } else {
    req->error = uv_new_sys_error(GetLastError());
  }

  memset(&req->overlapped, 0, sizeof(req->overlapped));

  /* Post completed */
  if (!PostQueuedCompletionStatus(LOOP->iocp,
                                0,
                                0,
                                &req->overlapped)) {
    uv_fatal_error(GetLastError(), "PostQueuedCompletionStatus");
  }

  return 0;
}


int uv_pipe_connect(uv_connect_t* req, uv_pipe_t* handle,
    const char* name, uv_connect_cb cb) {
  int errno, nameSize;
  HANDLE pipeHandle;

  handle->handle = INVALID_HANDLE_VALUE;

  uv_req_init((uv_req_t*) req);
  req->type = UV_CONNECT;
  req->handle = (uv_stream_t*) handle;
  req->cb = cb;

  /* Convert name to UTF16. */
  nameSize = uv_utf8_to_utf16(name, NULL, 0) * sizeof(wchar_t);
  handle->name = (wchar_t*)malloc(nameSize);
  if (!handle->name) {
    uv_fatal_error(ERROR_OUTOFMEMORY, "malloc");
  }

  if (!uv_utf8_to_utf16(name, handle->name, nameSize / sizeof(wchar_t))) {
    errno = GetLastError();
    goto error;
  }

  pipeHandle = CreateFileW(handle->name,
                          GENERIC_READ | GENERIC_WRITE,
                          0,
                          NULL,
                          OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED,
                          NULL);

  if (pipeHandle == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_PIPE_BUSY) {
      /* Wait for the server to make a pipe instance available. */
      if (!QueueUserWorkItem(&pipe_connect_thread_proc, req, WT_EXECUTELONGFUNCTION)) {
        errno = GetLastError();
        goto error;
      }

      return 0;
    }

    errno = GetLastError();
    goto error;
  }

  if (uv_set_pipe_handle((uv_pipe_t*)req->handle, pipeHandle)) {
    errno = GetLastError();
    goto error;
  }

  handle->handle = pipeHandle;

  req->error = uv_ok_;
  uv_insert_pending_req((uv_req_t*) req);
  handle->reqs_pending++;
  return 0;

error:
  if (handle->name) {
    free(handle->name);
    handle->name = NULL;
  }

  if (pipeHandle != INVALID_HANDLE_VALUE) {
    CloseHandle(pipeHandle);
  }
  uv_set_sys_error(errno);
  return -1;
}


/* Cleans up uv_pipe_t (server or connection) and all resources associated with it */
void close_pipe(uv_pipe_t* handle, int* status, uv_err_t* err) {
  int i;
  HANDLE pipeHandle;

  if (handle->name) {
    free(handle->name);
    handle->name = NULL;
  }

  if (handle->flags & UV_HANDLE_PIPESERVER) {
    for (i = 0; i < COUNTOF(handle->accept_reqs); i++) {
      pipeHandle = handle->accept_reqs[i].pipeHandle;
      if (pipeHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(pipeHandle);
        handle->accept_reqs[i].pipeHandle = INVALID_HANDLE_VALUE;
      }
    }

  } else if (handle->handle != INVALID_HANDLE_VALUE) {
    CloseHandle(handle->handle);
    handle->handle = INVALID_HANDLE_VALUE;
  }

  handle->flags |= UV_HANDLE_SHUT;
}


static void uv_pipe_queue_accept(uv_pipe_t* handle, uv_pipe_accept_t* req, BOOL firstInstance) {
  assert(handle->flags & UV_HANDLE_LISTENING);

  if (!firstInstance) {
    assert(req->pipeHandle == INVALID_HANDLE_VALUE);

    req->pipeHandle = CreateNamedPipeW(handle->name,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES,
                                       65536,
                                       65536,
                                       0,
                                       NULL);

    if (req->pipeHandle == INVALID_HANDLE_VALUE) {
      req->error = uv_new_sys_error(GetLastError());
      uv_insert_pending_req((uv_req_t*) req);
      handle->reqs_pending++;
      return;
    }

    if (uv_set_pipe_handle(handle, req->pipeHandle)) {
      CloseHandle(req->pipeHandle);
      req->pipeHandle = INVALID_HANDLE_VALUE;
      req->error = uv_new_sys_error(GetLastError());
      uv_insert_pending_req((uv_req_t*) req);
      handle->reqs_pending++;
      return;
    }
  }

  assert(req->pipeHandle != INVALID_HANDLE_VALUE);

  /* Prepare the overlapped structure. */
  memset(&(req->overlapped), 0, sizeof(req->overlapped));

  if (!ConnectNamedPipe(req->pipeHandle, &req->overlapped) && GetLastError() != ERROR_IO_PENDING) {
    if (GetLastError() == ERROR_PIPE_CONNECTED) {
      req->error = uv_ok_;
    } else {
      CloseHandle(req->pipeHandle);
      req->pipeHandle = INVALID_HANDLE_VALUE;
      /* Make this req pending reporting an error. */
      req->error = uv_new_sys_error(GetLastError());
    }
    uv_insert_pending_req((uv_req_t*) req);
    handle->reqs_pending++;
    return;
  }

  handle->reqs_pending++;
}


int uv_pipe_accept(uv_pipe_t* server, uv_pipe_t* client) {
  /* Find a connection instance that has been connected, but not yet accepted. */
  uv_pipe_accept_t* req = server->pending_accepts;

  if (!req) {
    /* No valid connections found, so we error out. */
    uv_set_sys_error(WSAEWOULDBLOCK);
    return -1;
  }

  /* Initialize the client handle and copy the pipeHandle to the client */
  uv_connection_init((uv_stream_t*) client);
  client->handle = req->pipeHandle;

  /* Prepare the req to pick up a new connection */
  server->pending_accepts = req->next_pending;
  req->next_pending = NULL;
  req->pipeHandle = INVALID_HANDLE_VALUE;

  if (!(server->flags & UV_HANDLE_CLOSING)) {
    uv_pipe_queue_accept(server, req, FALSE);
  }

  return 0;
}


/* Starts listening for connections for the given pipe. */
int uv_pipe_listen(uv_pipe_t* handle, uv_connection_cb cb) {
  int i, errno;

  if (handle->flags & UV_HANDLE_BIND_ERROR) {
    LOOP->last_error = handle->error;
    return -1;
  }

  if (!(handle->flags & UV_HANDLE_BOUND)) {
    uv_set_error(UV_ENOTCONN, 0);
    return -1;
  }

  if (handle->flags & UV_HANDLE_LISTENING ||
      handle->flags & UV_HANDLE_READING) {
    uv_set_error(UV_EALREADY, 0);
    return -1;
  }

  if (!(handle->flags & UV_HANDLE_PIPESERVER)) {
    uv_set_error(UV_ENOTSUP, 0);
    return -1;
  }

  handle->flags |= UV_HANDLE_LISTENING;
  handle->connection_cb = cb;

  /* First pipe handle should have already been created in uv_pipe_bind */
  assert(handle->accept_reqs[0].pipeHandle != INVALID_HANDLE_VALUE);

  for (i = 0; i < COUNTOF(handle->accept_reqs); i++) {
    uv_pipe_queue_accept(handle, &handle->accept_reqs[i], i == 0);
  }

  return 0;
}


static void uv_pipe_queue_read(uv_pipe_t* handle) {
  uv_req_t* req;
  int result;

  assert(handle->flags & UV_HANDLE_READING);
  assert(!(handle->flags & UV_HANDLE_READ_PENDING));

  assert(handle->handle != INVALID_HANDLE_VALUE);

  req = &handle->read_req;
  memset(&req->overlapped, 0, sizeof(req->overlapped));

  /* Do 0-read */
  result = ReadFile(handle->handle,
                    &uv_zero_,
                    0,
                    NULL,
                    &req->overlapped);

  if (!result && GetLastError() != ERROR_IO_PENDING) {
    /* Make this req pending reporting an error. */
    req->error = uv_new_sys_error(WSAGetLastError());
    uv_insert_pending_req(req);
    handle->reqs_pending++;
    return;
  }

  handle->flags |= UV_HANDLE_READ_PENDING;
  handle->reqs_pending++;
}


int uv_pipe_read_start(uv_pipe_t* handle, uv_alloc_cb alloc_cb, uv_read_cb read_cb) {
  if (!(handle->flags & UV_HANDLE_CONNECTION)) {
    uv_set_error(UV_EINVAL, 0);
    return -1;
  }

  if (handle->flags & UV_HANDLE_READING) {
    uv_set_error(UV_EALREADY, 0);
    return -1;
  }

  if (handle->flags & UV_HANDLE_EOF) {
    uv_set_error(UV_EOF, 0);
    return -1;
  }

  handle->flags |= UV_HANDLE_READING;
  handle->read_cb = read_cb;
  handle->alloc_cb = alloc_cb;

  /* If reading was stopped and then started again, there could stell be a */
  /* read request pending. */
  if (!(handle->flags & UV_HANDLE_READ_PENDING))
    uv_pipe_queue_read(handle);

  return 0;
}


int uv_pipe_write(uv_write_t* req, uv_pipe_t* handle, uv_buf_t bufs[], int bufcnt,
    uv_write_cb cb) {
  int result;

  if (bufcnt != 1) {
    uv_set_error(UV_ENOTSUP, 0);
    return -1;
  }

  assert(handle->handle != INVALID_HANDLE_VALUE);

  if (!(handle->flags & UV_HANDLE_CONNECTION)) {
    uv_set_error(UV_EINVAL, 0);
    return -1;
  }

  if (handle->flags & UV_HANDLE_SHUTTING) {
    uv_set_error(UV_EOF, 0);
    return -1;
  }

  uv_req_init((uv_req_t*) req);
  req->type = UV_WRITE;
  req->handle = (uv_stream_t*) handle;
  req->cb = cb;
  memset(&req->overlapped, 0, sizeof(req->overlapped));

  result = WriteFile(handle->handle,
                     bufs[0].base,
                     bufs[0].len,
                     NULL,
                     &req->overlapped);

  if (!result && GetLastError() != WSA_IO_PENDING) {
    uv_set_sys_error(GetLastError());
    return -1;
  }

  if (result) {
    /* Request completed immediately. */
    req->queued_bytes = 0;
  } else {
    /* Request queued by the kernel. */
    req->queued_bytes = uv_count_bufs(bufs, bufcnt);
    handle->write_queue_size += req->queued_bytes;
  }

  handle->reqs_pending++;
  handle->write_reqs_pending++;

  return 0;
}


void uv_process_pipe_read_req(uv_pipe_t* handle, uv_req_t* req) {
  DWORD bytes, avail;
  uv_buf_t buf;

  assert(handle->type == UV_NAMED_PIPE);

  handle->flags &= ~UV_HANDLE_READ_PENDING;

  if (req->error.code != UV_OK) {
    /* An error occurred doing the 0-read. */
    if (handle->flags & UV_HANDLE_READING) {
      /* Stop reading and report error. */
      handle->flags &= ~UV_HANDLE_READING;
      LOOP->last_error = req->error;
      buf.base = 0;
      buf.len = 0;
      handle->read_cb((uv_stream_t*)handle, -1, buf);
    }
  } else {
    /* Do non-blocking reads until the buffer is empty */
    while (handle->flags & UV_HANDLE_READING) {
      if (!PeekNamedPipe(handle->handle,
                         NULL,
                         0,
                         NULL,
                         &avail,
                         NULL)) {
        uv_set_sys_error(GetLastError());
        buf.base = 0;
        buf.len = 0;
        handle->read_cb((uv_stream_t*)handle, -1, buf);
        break;
      }

      buf = handle->alloc_cb((uv_stream_t*)handle, avail);
      assert(buf.len > 0);

      if (ReadFile(handle->handle,
                   buf.base,
                   buf.len,
                   &bytes,
                   NULL)) {
        if (bytes > 0) {
          /* Successful read */
          handle->read_cb((uv_stream_t*)handle, bytes, buf);
          /* Read again only if bytes == buf.len */
          if (bytes <= buf.len) {
            break;
          }
        } else {
          /* Connection closed */
          handle->flags &= ~UV_HANDLE_READING;
          handle->flags |= UV_HANDLE_EOF;
          LOOP->last_error.code = UV_EOF;
          LOOP->last_error.sys_errno_ = ERROR_SUCCESS;
          handle->read_cb((uv_stream_t*)handle, -1, buf);
          break;
        }
      } else {
        /* Ouch! serious error. */
        uv_set_sys_error(GetLastError());
        handle->read_cb((uv_stream_t*)handle, -1, buf);
        break;
      }
    }

    /* Post another 0-read if still reading and not closing. */
    if ((handle->flags & UV_HANDLE_READING) &&
        !(handle->flags & UV_HANDLE_READ_PENDING)) {
      uv_pipe_queue_read(handle);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_pipe_write_req(uv_pipe_t* handle, uv_write_t* req) {
  assert(handle->type == UV_NAMED_PIPE);

  handle->write_queue_size -= req->queued_bytes;

  if (req->cb) {
    LOOP->last_error = req->error;
    ((uv_write_cb)req->cb)(req, LOOP->last_error.code == UV_OK ? 0 : -1);
  }

  handle->write_reqs_pending--;
  if (handle->write_reqs_pending == 0 &&
      handle->flags & UV_HANDLE_SHUTTING) {
    uv_want_endgame((uv_handle_t*)handle);
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_pipe_accept_req(uv_pipe_t* handle, uv_req_t* raw_req) {
  uv_pipe_accept_t* req = (uv_pipe_accept_t*) raw_req;

  assert(handle->type == UV_NAMED_PIPE);

  if (req->error.code == UV_OK) {
    assert(req->pipeHandle != INVALID_HANDLE_VALUE);

    req->next_pending = handle->pending_accepts;
    handle->pending_accepts = req;

    if (handle->connection_cb) {
      handle->connection_cb((uv_handle_t*)handle, 0);
    }
  } else {
    if (req->pipeHandle != INVALID_HANDLE_VALUE) {
      CloseHandle(req->pipeHandle);
      req->pipeHandle = INVALID_HANDLE_VALUE;
    }
    if (!(handle->flags & UV_HANDLE_CLOSING)) {
      uv_pipe_queue_accept(handle, req, FALSE);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}


void uv_process_pipe_connect_req(uv_pipe_t* handle, uv_connect_t* req) {
  assert(handle->type == UV_NAMED_PIPE);

  if (req->cb) {
    if (req->error.code == UV_OK) {
      uv_connection_init((uv_stream_t*)handle);
      ((uv_connect_cb)req->cb)(req, 0);
    } else {
      LOOP->last_error = req->error;
      ((uv_connect_cb)req->cb)(req, -1);
    }
  }

  DECREASE_PENDING_REQ_COUNT(handle);
}
