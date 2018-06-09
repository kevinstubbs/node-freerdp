#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef _WIN32
#include <unistd.h>
#include <pthread.h>
#include <sys/select.h>
#else
#include <winsock2.h>
#include <Windows.h>
#include <ws2tcpip.h>
#endif

#include "rdp.h"

#include "generator.h"
#include "context.h"
#include "cliprdr.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <nan.h>

#include <freerdp/freerdp.h>
#include <freerdp/constants.h>
#include <freerdp/gdi/gdi.h>
#include <freerdp/utils/event.h>
#include <freerdp/client/file.h>
#include <freerdp/client/cmdline.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/client/channels.h>
#include <freerdp/channels/channels.h>

#include <winpr/crt.h>
#include <winpr/synch.h>

#include <nan.h>

using Nan::Callback;

using v8::Object;
using v8::Array;
using v8::Number;
using v8::Value;
using v8::Local;
using v8::String;
using Nan::New;
using Nan::Null;

struct thread_data
{
  freerdp* instance;
  bool stopping;
};

thread_data** sessions;
int sessionCount = 0;

bool node_freerdp_global_init = false;

int add_session(thread_data* session)
{
  if(sessions == NULL) {
    sessionCount = 1;
    sessions = (thread_data **)malloc(sizeof(thread_data *));
  } else {
    sessionCount += 1;
    thread_data **newSessions = (thread_data **)malloc(sizeof(thread_data *) * sessionCount);
    memcpy(newSessions, sessions, sizeof(thread_data *) * (sessionCount - 1));
    free(sessions);
    sessions = newSessions;
  }

  sessions[sessionCount - 1] = session;

  return sessionCount - 1;
}

int node_context_new(freerdp* instance, rdpContext* context)
{
  context->channels = freerdp_channels_new();
  return 0;
}

void node_context_free(freerdp* instance, rdpContext* context)
{

}


void node_begin_paint(rdpContext* context)
{
  rdpGdi* gdi = context->gdi;
  gdi->primary->hdc->hwnd->invalid->null = 1;
}

struct connect_args {};

Local<Array> connect_args_parser(void *generic) {
  connect_args *args = static_cast<connect_args *>(generic);

  Local<Array> argv = New<Array>();

  free(args);

  return argv;
}

const struct GeneratorType CONNECT_GENERATOR_TYPE {
  .name = "connect",
  .arg_parser = connect_args_parser
};

Local<Array> close_args_parser(void *generic) {
  connect_args *args = static_cast<connect_args *>(generic);

  Local<Array> argv = New<Array>();

  free(args);

  return argv;
}

struct close_args {};

const struct GeneratorType CLOSE_GENERATOR_TYPE {
  .name = "close",
  .arg_parser = close_args_parser
};

struct draw_args {
  int x;
  int y;
  int w;
  int h;
  int bpp;
  BYTE* buffer;
};

Local<Array> draw_args_parser(void *generic) {
  draw_args *args = static_cast<draw_args *>(generic);

  Local<Object> obj = New<Object>();

  obj->Set(New<String>("x").ToLocalChecked(), New<Number>(args->x));
  obj->Set(New<String>("y").ToLocalChecked(), New<Number>(args->y));
  obj->Set(New<String>("w").ToLocalChecked(), New<Number>(args->w));
  obj->Set(New<String>("h").ToLocalChecked(), New<Number>(args->h));
  obj->Set(New<String>("bpp").ToLocalChecked(), New<Number>(args->bpp));

  int size = args->w * args->h * args->bpp;

  Nan::MaybeLocal<v8::Object> buffer = Nan::CopyBuffer((const char *)args->buffer, size);
  obj->Set(New<String>("buffer").ToLocalChecked(), buffer.ToLocalChecked());

  Local<Array> argv = New<Array>();
  argv->Set(0, obj);

  delete[] args->buffer;
  delete args;

  return argv;
}

const struct GeneratorType DRAW_GENERATOR_TYPE {
  .name = "bitmap",
  .arg_parser = draw_args_parser
};

void node_end_paint(rdpContext* context)
{
  rdpGdi* gdi = context->gdi;
  if (gdi->primary->hdc->hwnd->invalid->null)
    return;

  draw_args *args = new draw_args;
  args->x = gdi->primary->hdc->hwnd->invalid->x;
  args->y = gdi->primary->hdc->hwnd->invalid->y;
  args->w = gdi->primary->hdc->hwnd->invalid->w;
  args->h = gdi->primary->hdc->hwnd->invalid->h;

  args->bpp = gdi->bytesPerPixel;

  int size = args->w * args->h * args->bpp;
  args->buffer = new BYTE[size];

  // copy only lines relevant to updated chunk
  int dest_pos = 0;
  int dest_line_width = args->w * args->bpp;
  for(int i = args->y; i < args->y + args->h; i++) {
    // memcopy only columns that are relevant
    int start_pos = (i * gdi->width * args->bpp) + (args->x * args->bpp);
    BYTE* src = &gdi->primary_buffer[start_pos];
    BYTE* dest = &args->buffer[dest_pos];
    memcpy(dest, src, dest_line_width);
    dest_pos += dest_line_width;
  }

  nodeContext *nc = (nodeContext*)context;

  generator_emit(nc->generatorContext, &DRAW_GENERATOR_TYPE, args);
}

/* returns rdpChannel for the channel id passed in */
rdpChannel* node_channels_find_channel_by_id(rdpChannels* channels, rdpSettings* settings, int channel_id, int* pindex)
{
  int index;
  int count;
  rdpChannel* channel;

  count = settings->ChannelCount;

  for (index = 0; index < count; index++)
  {
    channel = &settings->ChannelDefArray[index];

    if (channel->ChannelId == channel_id)
    {
      if (pindex != 0)
        *pindex = index;

      return channel;
    }
  }

  return NULL;
}

int node_receive_channel_data(freerdp* instance, int channelId, BYTE* data, int size, int flags, int total_size)
{
  return freerdp_channels_data(instance, channelId, data, size, flags, total_size);
}

void node_process_channel_event(rdpChannels* channels, freerdp* instance)
{
  wMessage* event;

  while((event = freerdp_channels_pop_event(channels)) != NULL) {
    switch (GetMessageClass(event->id))
    {
      case CliprdrChannel_Class:
        node_process_cliprdr_event(instance, event);
        break;
      default:
        printf("node_process_channel_event: unknown event type %d\n", GetMessageType(event->id));
        break;
    }

    freerdp_event_free(event);
  }
}

BOOL node_pre_connect(freerdp* instance)
{
  nodeInfo* nodei;
  nodeContext* context;
  rdpSettings* settings;

  context = (nodeContext*) instance->context;

  nodei = (nodeInfo*) malloc(sizeof(nodeInfo));
  ZeroMemory(nodei, sizeof(nodeInfo));

  context->nodei = nodei;

  settings = instance->settings;

  settings->OrderSupport[NEG_DSTBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_PATBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_SCRBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_OPAQUE_RECT_INDEX] = TRUE;
  settings->OrderSupport[NEG_DRAWNINEGRID_INDEX] = TRUE;
  settings->OrderSupport[NEG_MULTIDSTBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MULTIPATBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MULTISCRBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MULTIOPAQUERECT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MULTI_DRAWNINEGRID_INDEX] = TRUE;
  settings->OrderSupport[NEG_LINETO_INDEX] = TRUE;
  settings->OrderSupport[NEG_POLYLINE_INDEX] = TRUE;
  settings->OrderSupport[NEG_MEMBLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_MEM3BLT_INDEX] = TRUE;
  settings->OrderSupport[NEG_SAVEBITMAP_INDEX] = TRUE;
  settings->OrderSupport[NEG_GLYPH_INDEX_INDEX] = TRUE;
  settings->OrderSupport[NEG_FAST_INDEX_INDEX] = TRUE;
  settings->OrderSupport[NEG_FAST_GLYPH_INDEX] = TRUE;
  settings->OrderSupport[NEG_POLYGON_SC_INDEX] = TRUE;
  settings->OrderSupport[NEG_POLYGON_CB_INDEX] = TRUE;
  settings->OrderSupport[NEG_ELLIPSE_SC_INDEX] = TRUE;
  settings->OrderSupport[NEG_ELLIPSE_CB_INDEX] = TRUE;

  freerdp_channels_pre_connect(instance->context->channels, instance);

  return TRUE;
}

BOOL node_post_connect(freerdp* instance)
{
  rdpGdi* gdi;

  //gdi_init(instance, CLRCONV_ALPHA | CLRCONV_INVERT | CLRBUF_16BPP | CLRBUF_32BPP, NULL);
  gdi_init(instance, CLRCONV_ALPHA | CLRBUF_32BPP, NULL);
  gdi = instance->context->gdi;

  instance->update->BeginPaint = node_begin_paint;
  instance->update->EndPaint = node_end_paint;

  node_cliprdr_init(instance);

  freerdp_channels_post_connect(instance->context->channels, instance);

  nodeContext *nc = (nodeContext*)instance->context;
  connect_args *args = (connect_args *)malloc(sizeof(connect_args));
  generator_emit(nc->generatorContext, &CONNECT_GENERATOR_TYPE, args);

  return TRUE;
}

int tfreerdp_run(thread_data* data)
{
  int i;
  int fds;
  int max_fds;
  int rcount;
  int wcount;
  void* rfds[32];
  void* wfds[32];
  fd_set rfds_set;
  fd_set wfds_set;
  rdpChannels* channels;
  struct timeval tv = {0, 100000}; // timeout fd wait every 100ms

  freerdp* instance = data->instance;

  ZeroMemory(rfds, sizeof(rfds));
  ZeroMemory(wfds, sizeof(wfds));

  channels = instance->context->channels;

  if (!freerdp_connect(instance)) {
    return 0;
  }

  while (1)
  {
    rcount = 0;
    wcount = 0;

    if (freerdp_get_fds(instance, rfds, &rcount, wfds, &wcount) != TRUE)
    {
      printf("Failed to get FreeRDP file descriptor\n");
      break;
    }
    if (freerdp_channels_get_fds(channels, instance, rfds, &rcount, wfds, &wcount) != TRUE)
    {
      printf("Failed to get channel manager file descriptor\n");
      break;
    }

    max_fds = 0;
    FD_ZERO(&rfds_set);
    FD_ZERO(&wfds_set);

    for (i = 0; i < rcount; i++)
    {
      fds = (int)(long)(rfds[i]);

      if (fds > max_fds)
        max_fds = fds;

      FD_SET(fds, &rfds_set);
    }

    if (max_fds == 0)
      break;

    if (select(max_fds + 1, &rfds_set, &wfds_set, NULL, &tv) == -1)
    {
      /* these are not really errors */
      if (!((errno == EAGAIN) ||
        (errno == EWOULDBLOCK) ||
        (errno == EINPROGRESS) ||
        (errno == EINTR))) /* signal occurred */
      {
        printf("tfreerdp_run: select failed\n");
        break;
      }
    }

    if (freerdp_check_fds(instance) != TRUE)
    {
      printf("Failed to check FreeRDP file descriptor\n");
      break;
    }
    if (freerdp_channels_check_fds(channels, instance) != TRUE)
    {
      printf("Failed to check channel manager file descriptor\n");
      break;
    }
    node_process_channel_event(channels, instance);

    if (data->stopping) { // thread signaled to shutdown
      break;
    }
  }

  nodeContext *nc = (nodeContext*)instance->context;
  close_args *args = (close_args *)malloc(sizeof(close_args));
  generator_emit(nc->generatorContext, &CLOSE_GENERATOR_TYPE, args);

  node_cliprdr_uninit(instance);

  free(nc->generatorContext);

  freerdp_free(instance);

  return 0;
}

void* thread_func(void* param)
{
  struct thread_data* data;
  data = (struct thread_data*) param;

  tfreerdp_run(data);

  free(data);

  pthread_detach(pthread_self());

  return NULL;
}

int node_freerdp_connect(int argc, char* argv[], Callback *callback)
{
  int status;
  pthread_t thread;
  freerdp* instance;
  rdpChannels* channels;
  struct thread_data* data;
  rdpSettings* settings = nullptr;
  nodeContext* nContext;

  if (!node_freerdp_global_init) {
    node_freerdp_global_init = true;
    freerdp_channels_global_init();
  }

  instance = freerdp_new();
  instance->PreConnect = node_pre_connect;
  instance->PostConnect = node_post_connect;
  instance->ReceiveChannelData = node_receive_channel_data;

  instance->ContextSize = sizeof(nodeContext);
  instance->ContextNew = node_context_new;
  instance->ContextFree = node_context_free;
  freerdp_context_new(instance);

  nContext = (nodeContext*)instance->context;
  nContext->generatorContext = new GeneratorContext;
  nContext->generatorContext->callback = callback;

  channels = instance->context->channels;

  status = freerdp_client_parse_command_line_arguments(argc, argv, instance->settings);

  status = freerdp_client_command_line_status_print(argc, argv, settings, status);

  if (status < 0)
    exit(0);

  freerdp_client_load_addins(instance->context->channels, instance->settings);

  data = (struct thread_data*) malloc(sizeof(struct thread_data));
  ZeroMemory(data, sizeof(sizeof(struct thread_data)));

  data->instance = instance;
  data->stopping = false;

  pthread_create(&thread, 0, thread_func, data);

  int index = add_session(data);

  return index;
}

void node_freerdp_send_key_event_scancode(int session_index, int code, int pressed)
{
  thread_data* session = sessions[session_index];
  freerdp* instance = session->instance;
  rdpInput* input = instance->input;

  freerdp_input_send_keyboard_event_ex(input, pressed, code);
}

void node_freerdp_send_pointer_event(int session_index, int flags, int x, int y)
{
  thread_data* session = sessions[session_index];
  freerdp* instance = session->instance;
  rdpInput* input = instance->input;

  input->MouseEvent(input, flags, x, y);
}

void node_freerdp_set_clipboard(int session_index, void* data, int len)
{
  thread_data* session = sessions[session_index];
  freerdp* instance = session->instance;

  node_process_cliprdr_set_clipboard_data(instance, data, len);
}

void node_freerdp_close(int session_index)
{
  // NOTE: Doesn't block on closed session, will send closed event when completed
  thread_data *session = sessions[session_index];
  session->stopping = true;
}
