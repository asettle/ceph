// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "common/Formatter.h"
#include "common/debug.h"
#include "common/errno.h"
#include "include/stringify.h"
#include "cls/rbd/cls_rbd_client.h"
#include "common/Timer.h"
#include "common/WorkQueue.h"
#include "journal/Journaler.h"
#include "journal/ReplayHandler.h"
#include "librbd/ExclusiveLock.h"
#include "librbd/ImageCtx.h"
#include "librbd/ImageState.h"
#include "librbd/Journal.h"
#include "librbd/Operations.h"
#include "librbd/Utils.h"
#include "librbd/journal/Replay.h"
#include "ImageReplayer.h"
#include "ImageSync.h"
#include "Threads.h"
#include "tools/rbd_mirror/image_replayer/BootstrapRequest.h"
#include "tools/rbd_mirror/image_replayer/CloseImageRequest.h"
#include "tools/rbd_mirror/image_replayer/OpenLocalImageRequest.h"

#define dout_subsys ceph_subsys_rbd_mirror
#undef dout_prefix
#define dout_prefix *_dout << "rbd-mirror: " << *this << "::" << __func__ << ": "

using std::map;
using std::string;
using std::unique_ptr;
using std::vector;

namespace rbd {
namespace mirror {

using librbd::util::create_context_callback;
using namespace rbd::mirror::image_replayer;

template <typename I>
std::ostream &operator<<(std::ostream &os,
                         const typename ImageReplayer<I>::State &state);

namespace {

template <typename I>
struct ReplayHandler : public ::journal::ReplayHandler {
  ImageReplayer<I> *replayer;
  ReplayHandler(ImageReplayer<I> *replayer) : replayer(replayer) {}

  virtual void get() {}
  virtual void put() {}

  virtual void handle_entries_available() {
    replayer->handle_replay_ready();
  }
  virtual void handle_complete(int r) {
    replayer->handle_replay_complete(r);
  }
};

class ImageReplayerAdminSocketCommand {
public:
  virtual ~ImageReplayerAdminSocketCommand() {}
  virtual bool call(Formatter *f, stringstream *ss) = 0;
};

template <typename I>
class StatusCommand : public ImageReplayerAdminSocketCommand {
public:
  explicit StatusCommand(ImageReplayer<I> *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    replayer->print_status(f, ss);
    return true;
  }

private:
  ImageReplayer<I> *replayer;
};

template <typename I>
class FlushCommand : public ImageReplayerAdminSocketCommand {
public:
  explicit FlushCommand(ImageReplayer<I> *replayer) : replayer(replayer) {}

  bool call(Formatter *f, stringstream *ss) {
    C_SaferCond cond;
    replayer->flush(&cond);
    int r = cond.wait();
    if (r < 0) {
      *ss << "flush: " << cpp_strerror(r);
      return false;
    }
    return true;
  }

private:
  ImageReplayer<I> *replayer;
};

template <typename I>
class ImageReplayerAdminSocketHook : public AdminSocketHook {
public:
  ImageReplayerAdminSocketHook(CephContext *cct, const std::string &name,
			       ImageReplayer<I> *replayer) :
    admin_socket(cct->get_admin_socket()) {
    std::string command;
    int r;

    command = "rbd mirror status " + name;
    r = admin_socket->register_command(command, command, this,
				       "get status for rbd mirror " + name);
    if (r == 0) {
      commands[command] = new StatusCommand<I>(replayer);
    }

    command = "rbd mirror flush " + name;
    r = admin_socket->register_command(command, command, this,
				       "flush rbd mirror " + name);
    if (r == 0) {
      commands[command] = new FlushCommand<I>(replayer);
    }
  }

  ~ImageReplayerAdminSocketHook() {
    for (Commands::const_iterator i = commands.begin(); i != commands.end();
	 ++i) {
      (void)admin_socket->unregister_command(i->first);
      delete i->second;
    }
  }

  bool call(std::string command, cmdmap_t& cmdmap, std::string format,
	    bufferlist& out) {
    Commands::const_iterator i = commands.find(command);
    assert(i != commands.end());
    Formatter *f = Formatter::create(format);
    stringstream ss;
    bool r = i->second->call(f, &ss);
    delete f;
    out.append(ss);
    return r;
  }

private:
  typedef std::map<std::string, ImageReplayerAdminSocketCommand*> Commands;

  AdminSocket *admin_socket;
  Commands commands;
};

} // anonymous namespace

template <typename I>
ImageReplayer<I>::ImageReplayer(Threads *threads, RadosRef local, RadosRef remote,
			     const std::string &mirror_uuid,
			     int64_t local_pool_id,
			     int64_t remote_pool_id,
			     const std::string &remote_image_id,
                             const std::string &global_image_id) :
  m_threads(threads),
  m_local(local),
  m_remote(remote),
  m_mirror_uuid(mirror_uuid),
  m_remote_pool_id(remote_pool_id),
  m_local_pool_id(local_pool_id),
  m_remote_image_id(remote_image_id),
  m_global_image_id(global_image_id),
  m_name(stringify(remote_pool_id) + "/" + remote_image_id),
  m_lock("rbd::mirror::ImageReplayer " + stringify(remote_pool_id) + " " +
	 remote_image_id),
  m_state(STATE_UNINITIALIZED),
  m_local_image_ctx(nullptr),
  m_local_replay(nullptr),
  m_remote_journaler(nullptr),
  m_replay_handler(nullptr),
  m_asok_hook(nullptr)
{
}

template <typename I>
ImageReplayer<I>::~ImageReplayer()
{
  assert(m_local_image_ctx == nullptr);
  assert(m_local_replay == nullptr);
  assert(m_remote_journaler == nullptr);
  assert(m_replay_handler == nullptr);
  assert(m_on_start_finish == nullptr);
  assert(m_on_stop_finish == nullptr);
  delete m_asok_hook;
}

template <typename I>
void ImageReplayer<I>::start(Context *on_finish,
			     const BootstrapParams *bootstrap_params)
{
  assert(m_on_start_finish == nullptr);
  assert(m_on_stop_finish == nullptr);
  dout(20) << "on_finish=" << on_finish << dendl;

  {
    Mutex::Locker locker(m_lock);
    assert(is_stopped_());

    m_state = STATE_STARTING;
    m_on_start_finish = on_finish;
  }

  int r = m_remote->ioctx_create2(m_remote_pool_id, m_remote_ioctx);
  if (r < 0) {
    derr << "error opening ioctx for remote pool " << m_remote_pool_id
	 << ": " << cpp_strerror(r) << dendl;
    on_start_fail_start(r);
    return;
  }

  if (bootstrap_params != nullptr && !bootstrap_params->empty()) {
    m_local_image_name = bootstrap_params->local_image_name;
  }

  r = m_local->ioctx_create2(m_local_pool_id, m_local_ioctx);
  if (r < 0) {
    derr << "error opening ioctx for local pool " << m_local_pool_id
         << ": " << cpp_strerror(r) << dendl;
    on_start_fail_start(r);
    return;
  }

  CephContext *cct = static_cast<CephContext *>(m_local->cct());
  double commit_interval = cct->_conf->rbd_journal_commit_age;
  m_remote_journaler = new Journaler(m_threads->work_queue,
                                     m_threads->timer,
				     &m_threads->timer_lock, m_remote_ioctx,
				     m_remote_image_id, m_mirror_uuid,
                                     commit_interval);

  bootstrap();
}

template <typename I>
void ImageReplayer<I>::bootstrap() {
  dout(20) << "bootstrap params: "
	   << "local_image_name=" << m_local_image_name << dendl;

  // TODO: add a new bootstrap state and support canceling
  Context *ctx = create_context_callback<
    ImageReplayer, &ImageReplayer<I>::handle_bootstrap>(this);
  BootstrapRequest<I> *request = BootstrapRequest<I>::create(
    m_local_ioctx, m_remote_ioctx, &m_local_image_ctx,
    m_local_image_name, m_remote_image_id, m_global_image_id,
    m_threads->work_queue, m_threads->timer, &m_threads->timer_lock,
    m_mirror_uuid, m_remote_journaler, &m_client_meta, ctx);
  request->send();
}

template <typename I>
void ImageReplayer<I>::handle_bootstrap(int r) {
  dout(20) << "r=" << r << dendl;

  if (r < 0) {
    on_start_fail_start(r);
    return;
  } else if (on_start_interrupted()) {
    return;
  }

  {
    Mutex::Locker locker(m_lock);
    m_name = m_local_ioctx.get_pool_name() + "/" + m_local_image_ctx->name;

    CephContext *cct = static_cast<CephContext *>(m_local->cct());
    delete m_asok_hook;
    m_asok_hook = new ImageReplayerAdminSocketHook<I>(cct, m_name, this);
  }

  init_remote_journaler();
}

template <typename I>
void ImageReplayer<I>::init_remote_journaler() {
  dout(20) << dendl;

  Context *ctx = create_context_callback<
    ImageReplayer, &ImageReplayer<I>::handle_init_remote_journaler>(this);
  m_remote_journaler->init(ctx);
}

template <typename I>
void ImageReplayer<I>::handle_init_remote_journaler(int r) {
  dout(20) << "r=" << r << dendl;

  if (r < 0) {
    derr << "failed to initialize remote journal: " << cpp_strerror(r) << dendl;
    on_start_fail_start(r);
    return;
  } else if (on_start_interrupted()) {
    return;
  }

  start_replay();
}

template <typename I>
void ImageReplayer<I>::start_replay() {
  dout(20) << dendl;

  int r = m_local_image_ctx->journal->start_external_replay(&m_local_replay);
  if (r < 0) {
    derr << "error starting external replay on local image "
	 <<  m_local_image_id << ": " << cpp_strerror(r) << dendl;
    on_start_fail_start(r);
    return;
  }

  m_replay_handler = new ReplayHandler<I>(this);
  m_remote_journaler->start_live_replay(m_replay_handler,
					1 /* TODO: configurable */);

  dout(20) << "m_remote_journaler=" << *m_remote_journaler << dendl;

  assert(r == 0);

  Context *on_finish(nullptr);
  {
    Mutex::Locker locker(m_lock);
    if (m_stop_requested) {
      on_start_fail_start(-EINTR);
      return;
    }

    assert(m_state == STATE_STARTING);
    m_state = STATE_REPLAYING;
    std::swap(m_on_start_finish, on_finish);
  }

  dout(20) << "start succeeded" << dendl;
  if (on_finish != nullptr) {
    dout(20) << "on finish complete, r=" << r << dendl;
    on_finish->complete(r);
  }
}

template <typename I>
void ImageReplayer<I>::on_start_fail_start(int r)
{
  dout(20) << "r=" << r << dendl;

  FunctionContext *ctx = new FunctionContext(
    [this, r](int r1) {
      assert(r1 == 0);
      on_start_fail_finish(r);
    });

  m_threads->work_queue->queue(ctx, 0);
}

template <typename I>
void ImageReplayer<I>::on_start_fail_finish(int r)
{
  dout(20) << "r=" << r << dendl;

  if (m_remote_journaler) {
    if (m_remote_journaler->is_initialized()) {
      m_remote_journaler->shut_down();
    }
    delete m_remote_journaler;
    m_remote_journaler = nullptr;
  }

  if (m_local_replay) {
    shut_down_journal_replay(true);
    m_local_image_ctx->journal->stop_external_replay();
    m_local_replay = nullptr;
  }

  if (m_replay_handler) {
    delete m_replay_handler;
    m_replay_handler = nullptr;
  }

  if (m_local_image_ctx) {
    // TODO: switch to async close via CloseImageRequest
    m_local_image_ctx->state->close();
    m_local_image_ctx = nullptr;
  }

  m_local_ioctx.close();
  m_remote_ioctx.close();

  Context *on_start_finish(nullptr);
  Context *on_stop_finish(nullptr);
  {
    Mutex::Locker locker(m_lock);
    if (m_stop_requested) {
      assert(r == -EINTR);
      dout(20) << "start interrupted" << dendl;
      m_state = STATE_STOPPED;
      m_stop_requested = false;
    } else {
      assert(m_state == STATE_STARTING);
      dout(20) << "start failed" << dendl;
      m_state = STATE_UNINITIALIZED;
    }
    std::swap(m_on_start_finish, on_start_finish);
    std::swap(m_on_stop_finish, on_stop_finish);
  }

  if (on_start_finish != nullptr) {
    dout(20) << "on start finish complete, r=" << r << dendl;
    on_start_finish->complete(r);
  }
  if (on_stop_finish != nullptr) {
    dout(20) << "on stop finish complete, r=" << r << dendl;
    on_stop_finish->complete(0);
  }
}

template <typename I>
bool ImageReplayer<I>::on_start_interrupted()
{
  Mutex::Locker locker(m_lock);
  assert(m_state == STATE_STARTING);
  if (m_on_stop_finish == nullptr) {
    return false;
  }

  on_start_fail_start(-EINTR);
  return true;
}

template <typename I>
void ImageReplayer<I>::stop(Context *on_finish)
{
  dout(20) << "on_finish=" << on_finish << dendl;

  bool shut_down_replay = false;
  {
    Mutex::Locker locker(m_lock);
    assert(is_running_());

    if (!is_stopped_()) {
      if (m_state == STATE_STARTING) {
        dout(20) << "interrupting start" << dendl;
      } else {
        dout(20) << "interrupting replay" << dendl;
        shut_down_replay = true;
      }

      assert(m_on_stop_finish == nullptr);
      std::swap(m_on_stop_finish, on_finish);
      m_stop_requested = true;
    }
  }

  if (shut_down_replay) {
    on_stop_journal_replay_shut_down_start();
  } else if (on_finish != nullptr) {
    on_finish->complete(0);
  }
}

template <typename I>
void ImageReplayer<I>::on_stop_journal_replay_shut_down_start()
{
  dout(20) << "enter" << dendl;

  FunctionContext *ctx = new FunctionContext(
    [this](int r) {
      on_stop_journal_replay_shut_down_finish(r);
    });

  {
    Mutex::Locker locker(m_lock);

    // as we complete in-flight records, we might receive multiple stop requests
    if (m_state != STATE_REPLAYING) {
      return;
    }
    m_state = STATE_STOPPING;
    m_local_replay->shut_down(false, ctx);
  }
}

template <typename I>
void ImageReplayer<I>::on_stop_journal_replay_shut_down_finish(int r)
{
  dout(20) << "r=" << r << dendl;
  if (r < 0) {
    derr << "error flushing journal replay: " << cpp_strerror(r) << dendl;
  }

  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_STOPPING);
    m_local_image_ctx->journal->stop_external_replay();
    m_local_replay = nullptr;
  }

  on_stop_local_image_close_start();
}

template <typename I>
void ImageReplayer<I>::on_stop_local_image_close_start()
{
  dout(20) << "enter" << dendl;

  // close and delete the image (from outside the image's thread context)
  Context *ctx = create_context_callback<
    ImageReplayer, &ImageReplayer<I>::on_stop_local_image_close_finish>(this);
  CloseImageRequest<I> *request = CloseImageRequest<I>::create(
    &m_local_image_ctx, m_threads->work_queue, false, ctx);
  request->send();
}

template <typename I>
void ImageReplayer<I>::on_stop_local_image_close_finish(int r)
{
  dout(20) << "r=" << r << dendl;

  if (r < 0) {
    derr << "error closing local image: " << cpp_strerror(r) << dendl;
  }

  m_local_ioctx.close();

  m_remote_journaler->stop_replay();
  m_remote_journaler->shut_down();
  delete m_remote_journaler;
  m_remote_journaler = nullptr;

  delete m_replay_handler;
  m_replay_handler = nullptr;

  m_remote_ioctx.close();

  Context *on_finish(nullptr);

  {
    Mutex::Locker locker(m_lock);
    assert(m_state == STATE_STOPPING);

    m_state = STATE_STOPPED;
    m_stop_requested = false;
    std::swap(m_on_stop_finish, on_finish);
  }

  dout(20) << "stop complete" << dendl;

  if (on_finish != nullptr) {
    dout(20) << "on finish complete, r=" << r << dendl;
    on_finish->complete(r);
  }
}

template <typename I>
void ImageReplayer<I>::close_local_image(Context *on_finish)
{
  m_local_image_ctx->state->close(on_finish);
}

template <typename I>
void ImageReplayer<I>::handle_replay_ready()
{
  dout(20) << "enter" << dendl;
  if (on_replay_interrupted()) {
    return;
  }

  if (!m_remote_journaler->try_pop_front(&m_replay_entry)) {
    return;
  }

  // TODO
  process_entry();
}

template <typename I>
void ImageReplayer<I>::flush(Context *on_finish)
{
  dout(20) << "enter" << dendl;

  {
    Mutex::Locker locker(m_lock);
    if (m_state == STATE_REPLAYING || m_state == STATE_REPLAYING) {
      Context *ctx = new FunctionContext(
        [on_finish](int r) {
          if (on_finish != nullptr) {
            on_finish->complete(r);
          }
        });
      on_flush_local_replay_flush_start(ctx);
    }
  }

  if (on_finish) {
    on_finish->complete(0);
  }
}

template <typename I>
void ImageReplayer<I>::on_flush_local_replay_flush_start(Context *on_flush)
{
  dout(20) << "enter" << dendl;
  FunctionContext *ctx = new FunctionContext(
    [this, on_flush](int r) {
      on_flush_local_replay_flush_finish(on_flush, r);
    });

  assert(m_lock.is_locked());
  assert(m_state == STATE_REPLAYING);
  m_local_replay->flush(ctx);
}

template <typename I>
void ImageReplayer<I>::on_flush_local_replay_flush_finish(Context *on_flush,
                                                          int r)
{
  dout(20) << "r=" << r << dendl;
  if (r < 0) {
    derr << "error flushing local replay: " << cpp_strerror(r) << dendl;
    on_flush->complete(r);
    return;
  }

  on_flush_flush_commit_position_start(on_flush);
}

template <typename I>
void ImageReplayer<I>::on_flush_flush_commit_position_start(Context *on_flush)
{
  FunctionContext *ctx = new FunctionContext(
    [this, on_flush](int r) {
      on_flush_flush_commit_position_finish(on_flush, r);
    });

  m_remote_journaler->flush_commit_position(ctx);
}

template <typename I>
void ImageReplayer<I>::on_flush_flush_commit_position_finish(Context *on_flush,
                                                             int r)
{
  if (r < 0) {
    derr << "error flushing remote journal commit position: "
	 << cpp_strerror(r) << dendl;
  }

  dout(20) << "flush complete, r=" << r << dendl;
  on_flush->complete(r);
}

template <typename I>
bool ImageReplayer<I>::on_replay_interrupted()
{
  bool shut_down;
  {
    Mutex::Locker locker(m_lock);
    shut_down = m_stop_requested;
  }

  if (shut_down) {
    on_stop_journal_replay_shut_down_start();
  }
  return shut_down;
}

template <typename I>
void ImageReplayer<I>::print_status(Formatter *f, stringstream *ss)
{
  dout(20) << "enter" << dendl;

  Mutex::Locker l(m_lock);

  if (f) {
    f->open_object_section("image_replayer");
    f->dump_string("name", m_name);
    f->dump_string("state", to_string(m_state));
    f->close_section();
    f->flush(*ss);
  } else {
    *ss << m_name << ": state: " << to_string(m_state);
  }
}

template <typename I>
void ImageReplayer<I>::handle_replay_complete(int r)
{
  dout(20) << "r=" << r << dendl;
  if (r < 0) {
    derr << "replay encountered an error: " << cpp_strerror(r) << dendl;
  }

  {
    Mutex::Locker locker(m_lock);
    m_stop_requested = true;
  }
  on_replay_interrupted();
}

template <typename I>
void ImageReplayer<I>::replay_flush() {
  dout(20) << dendl;

  // TODO
}

template <typename I>
void ImageReplayer<I>::handle_replay_flush(int r) {
  dout(20) << "r=" << r << dendl;

  // TODO
}

template <typename I>
void ImageReplayer<I>::get_remote_tag() {
  dout(20) << dendl;

  // TODO
}

template <typename I>
void ImageReplayer<I>::handle_get_remote_tag(int r) {
  dout(20) << "r=" << r << dendl;

  // TODO
}

template <typename I>
void ImageReplayer<I>::allocate_local_tag() {
  dout(20) << dendl;

  // TODO
}

template <typename I>
void ImageReplayer<I>::handle_allocate_local_tag(int r) {
  dout(20) << "r=" << r << dendl;

  // TODO
}

template <typename I>
void ImageReplayer<I>::process_entry() {
  dout(20) << "processing entry tid=" << m_replay_entry.get_commit_tid()
           << dendl;

  bufferlist data = m_replay_entry.get_data();
  bufferlist::iterator it = data.begin();

  Context *on_ready = create_context_callback<
    ImageReplayer, &ImageReplayer<I>::handle_process_entry_ready>(this);
  Context *on_commit = new C_ReplayCommitted(this, std::move(m_replay_entry));
  m_local_replay->process(&it, on_ready, on_commit);
}

template <typename I>
void ImageReplayer<I>::handle_process_entry_ready(int r) {
  dout(20) << dendl;
  assert(r == 0);

  // attempt to process the next event
  handle_replay_ready();
}

template <typename I>
void ImageReplayer<I>::handle_process_entry_safe(const ReplayEntry& replay_entry,
                                                 int r) {
  dout(20) << "commit_tid=" << replay_entry.get_commit_tid() << ", r=" << r
	   << dendl;

  if (r < 0) {
    derr << "failed to commit journal event: " << cpp_strerror(r) << dendl;

    handle_replay_complete(r);
    return;
  }

  m_remote_journaler->committed(replay_entry);
}

template <typename I>
void ImageReplayer<I>::shut_down_journal_replay(bool cancel_ops)
{
  C_SaferCond cond;
  m_local_replay->shut_down(cancel_ops, &cond);
  int r = cond.wait();
  if (r < 0) {
    derr << "error flushing journal replay: " << cpp_strerror(r) << dendl;
  }
}

template <typename I>
std::string ImageReplayer<I>::to_string(const State state) {
  switch (state) {
  case ImageReplayer<I>::STATE_UNINITIALIZED:
    return "Uninitialized";
  case ImageReplayer<I>::STATE_STARTING:
    return "Starting";
  case ImageReplayer<I>::STATE_REPLAYING:
    return "Replaying";
  case ImageReplayer<I>::STATE_STOPPING:
    return "Stopping";
  case ImageReplayer<I>::STATE_STOPPED:
    return "Stopped";
  default:
    break;
  }
  return "Unknown(" + stringify(state) + ")";
}

template <typename I>
std::ostream &operator<<(std::ostream &os, const ImageReplayer<I> &replayer)
{
  os << "ImageReplayer[" << replayer.get_remote_pool_id() << "/"
     << replayer.get_remote_image_id() << "]";
  return os;
}

} // namespace mirror
} // namespace rbd

template class rbd::mirror::ImageReplayer<librbd::ImageCtx>;
