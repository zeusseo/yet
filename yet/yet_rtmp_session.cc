#include "yet_rtmp_session.h"
#include <asio.hpp>
#include "yet.hpp"
#include "yet_rtmp/yet_rtmp.hpp"
#include "yet_rtmp/yet_rtmp_amf_op.h"
#include "yet_rtmp/yet_rtmp_pack_op.h"
#include "chef_base/chef_stuff_op.hpp"

namespace yet {

//YET_LOG_DEBUG("{} len:{}", __func__, len);
#define SNIPPET_ENTER_CB \
  do { \
    if (!ec) { \
    } else { \
      YET_LOG_ERROR("[{}] {} ec:{}, len:{}", (void *)this, __func__, ec.message(), len); \
      if (ec == asio::error::eof) { \
        YET_LOG_INFO("[{}] close by peer.", (void *)this); \
        close(); \
      } else if (ec == asio::error::broken_pipe) { \
        YET_LOG_ERROR("[{}] broken pipe.", (void *)this); \
      } \
      return; \
    } \
  } while(0);


#define SNIPPET_KEEP_READ do { do_read(); return; } while(0);

#define SNIPPET_ASYNC_READ(pos, len, func)      asio::async_read(socket_, asio::buffer(pos, len), std::bind(func, shared_from_this(), _1, _2));
#define SNIPPET_ASYNC_READ_SOME(pos, len, func) socket_.async_read_some(asio::buffer(pos, len), std::bind(func, shared_from_this(), _1, _2));
#define SNIPPET_ASYNC_WRITE(pos, len, func)     asio::async_write(socket_, asio::buffer(pos, len), std::bind(func, shared_from_this(), _1, _2));

RtmpSession::RtmpSession(asio::ip::tcp::socket socket)
  : socket_(std::move(socket))
  , read_buf_(BUF_INIT_LEN_RTMP_EACH_READ, BUF_SHRINK_LEN_RTMP_EACH_READ)
  , write_buf_(BUF_INIT_LEN_RTMP_WRITE)
{
  YET_LOG_INFO("[{}] new rtmp session.", (void *)this);
}

RtmpSession::~RtmpSession() {
  YET_LOG_DEBUG("[{}] delete rtmp session.", (void *)this);
}

void RtmpSession::start() {
  do_read_c0c1();
}

void RtmpSession::do_read_c0c1() {
  read_buf_.reserve(RTMP_C0C1_LEN);
  auto self(shared_from_this());
  asio::async_read(socket_,
      asio::buffer(read_buf_.write_pos(), RTMP_C0C1_LEN),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        rtmp_handshake_.handle_c0c1(read_buf_.read_pos(), len);
        YET_LOG_INFO("[{}] ---->Handshake C0+C1", (void *)this);
        do_write_s0s1();
      });
}

void RtmpSession::do_write_s0s1() {
  YET_LOG_INFO("[{}] <----Handshake S0+S1", (void *)this);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(rtmp_handshake_.create_s0s1(), RTMP_S0S1_LEN),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        do_write_s2();
      });
}

void RtmpSession::do_write_s2() {
  YET_LOG_INFO("[{}] <----Handshake S2", (void *)this);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(rtmp_handshake_.create_s2(), RTMP_S2_LEN),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        do_read_c2();
      });
}

void RtmpSession::do_read_c2() {
  read_buf_.reserve(RTMP_C2_LEN);
  auto self(shared_from_this());
  asio::async_read(socket_,
      asio::buffer(read_buf_.write_pos(), RTMP_C2_LEN),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        YET_LOG_INFO("[{}] ---->Handshake C2", (void *)this);
        do_read();
      });
}

void RtmpSession::do_read() {
  read_buf_.reserve(BUF_INIT_LEN_RTMP_EACH_READ);
  SNIPPET_ASYNC_READ_SOME(read_buf_.write_pos(), BUF_INIT_LEN_RTMP_EACH_READ, &RtmpSession::read_cb);
}

void RtmpSession::read_cb(ErrorCode ec, std::size_t len) {
  SNIPPET_ENTER_CB;

  read_buf_.seek_write_pos(len);
  //YET_LOG_DEBUG("[{}] > read_cb. read len:{}, readable_size:{}", (void *)this, len, read_buf_.readable_size());

  for (; read_buf_.readable_size() > 0; ) {
    //YET_LOG_DEBUG("[{}] > message parse loop. read_buf readable_size:{}", (void *)this, read_buf_.readable_size());
    uint8_t *p = read_buf_.read_pos();

    if (!header_done_) {
      auto readable_size = read_buf_.readable_size();

      // 5.3.1.1. Chunk Basic Header 1,2,3bytes
      int basic_header_len;
      uint8_t fmt;
      fmt = (*p >> 6) & 0x03;    // 2 bits
      curr_csid_ = *p & 0x3F; // 6 bits
      if (curr_csid_ == 0) {
        basic_header_len = 2;
        if (readable_size < 2) { SNIPPET_KEEP_READ; }

        curr_csid_ = 64 + *(p+1);
      } else if (curr_csid_ == 1) {
        basic_header_len = 3;
        if (readable_size < 3) { SNIPPET_KEEP_READ; }

        curr_csid_ = 64 + *(p+1) + (*(p+2) * 256);
      } else if (curr_csid_ == 2) {
        basic_header_len = 1;
      } else if (curr_csid_ < 64) {
        basic_header_len = 1;
      } else {
        // erase compiler warning
        basic_header_len = -1;
        YET_LOG_ERROR("{}", curr_csid_);
      }

      auto stream = get_or_create_stream(curr_csid_);

      //YET_LOG_DEBUG("[{}] parsed basic header. {} fmt:{}, csid:{}, basic_header_len:{}",
      //              (void *)this, (unsigned char)*p, fmt, curr_csid_, basic_header_len);

      p += basic_header_len;
      readable_size -= basic_header_len;

      // 5.3.1.2. Chunk Message Header 11,7,3,0bytes
      if (fmt == 0) {
        if (readable_size < RTMP_FMT_2_MSG_HEADER_LEN[fmt]) { SNIPPET_KEEP_READ; }

        p = AmfOp::decode_int24(p, 3, (int *)&stream->header.timestamp, nullptr);
        stream->timestamp_abs = stream->header.timestamp;
        p = AmfOp::decode_int24(p, 3, (int *)&stream->msg_len, nullptr);
        stream->header.msg_type_id = *p++;
        p = AmfOp::decode_int32_le(p, 4, (int *)&stream->header.msg_stream_id, nullptr);
      } else if (fmt == 1) {
        if (readable_size < RTMP_FMT_2_MSG_HEADER_LEN[fmt]) { SNIPPET_KEEP_READ; }

        p = AmfOp::decode_int24(p, 3, (int *)&stream->header.timestamp, nullptr);
        stream->timestamp_abs += stream->header.timestamp;
        p = AmfOp::decode_int24(p, 3, (int *)&stream->msg_len, nullptr);
        stream->header.msg_type_id = *p++;
      } else if (fmt == 2) {
        if (readable_size < RTMP_FMT_2_MSG_HEADER_LEN[fmt]) { SNIPPET_KEEP_READ; }

        p = AmfOp::decode_int24(p, 3, (int *)&stream->header.timestamp, nullptr);
        stream->timestamp_abs += stream->header.timestamp;
      } else if (fmt == 3) {
        // noop
      }

      readable_size -= RTMP_FMT_2_MSG_HEADER_LEN[fmt];

      // 5.3.1.3 Extended Timestamp
      bool has_ext_ts;
      has_ext_ts = stream->header.timestamp == RTMP_MAX_TIMESTAMP_IN_MSG_HEADER;
      if (has_ext_ts) {
        if (readable_size < 4) { SNIPPET_KEEP_READ; }

        p = AmfOp::decode_int32(p, 4, (int *)&stream->header.timestamp, nullptr);
        if (fmt == 0) {
          stream->timestamp_abs = stream->header.timestamp;
        } else if (fmt == 1 || fmt == 2) {
          stream->timestamp_abs += stream->header.timestamp;
        } else {
          // noop
        }

        readable_size -= 4;
      }

      header_done_ = true;
      read_buf_.erase(basic_header_len + RTMP_FMT_2_MSG_HEADER_LEN[fmt] + (has_ext_ts ? 4 : 0));

      //YET_LOG_DEBUG("[{}] parsed chunk message header. msg_header_len:{}, timestamp:{} {}, msg_len:{}, msg_type_id:{}, msg_stream_id:{}",
      //              (void *)this, RTMP_FMT_2_MSG_HEADER_LEN[fmt], stream->header.timestamp, stream->timestamp_abs, stream->msg_len, stream->header.msg_type_id, stream->header.msg_stream_id);
    }

    curr_stream_ = get_or_create_stream(curr_csid_);

    std::size_t needed_size;
    if (curr_stream_->msg_len <= peer_chunk_size_) {
      needed_size = curr_stream_->msg_len;
    } else {
      std::size_t whole_needed = curr_stream_->msg_len - curr_stream_->msg->readable_size();
      needed_size = std::min(whole_needed, peer_chunk_size_);
    }

    if (read_buf_.readable_size() < needed_size) { SNIPPET_KEEP_READ; }

    curr_stream_->msg->append(read_buf_.read_pos(), needed_size);
    read_buf_.erase(needed_size);

    if (curr_stream_->msg->readable_size() == curr_stream_->msg_len) {
      complete_message_handler();
      curr_stream_->msg->clear();
    }
    YET_LOG_ASSERT(curr_stream_->msg->readable_size() <= curr_stream_->msg_len, "invalid readable size. {} {}",
                   curr_stream_->msg->readable_size(), curr_stream_->msg_len);

    header_done_ = false;
  }

  do_read();
}

void RtmpSession::complete_message_handler() {
  //YET_LOG_DEBUG("[{}] > complete message handler. type:{}", (void *)this, curr_stream_->header.msg_type_id);
  switch (curr_stream_->header.msg_type_id) {
  case RTMP_MSG_TYPE_ID_SET_CHUNK_SIZE:
  case RTMP_MSG_TYPE_ID_ABORT:
  case RTMP_MSG_TYPE_ID_ACK:
  case RTMP_MSG_TYPE_ID_WIN_ACK_SIZE:
  case RTMP_MSG_TYPE_ID_BANDWIDTH:
    protocol_control_message_handler();
    break;
  case RTMP_MSG_TYPE_ID_COMMAND_MESSAGE_AMF0:
    command_message_handler();
    break;
  case RTMP_MSG_TYPE_ID_DATA_MESSAGE_AMF0:
    data_message_handler();
    break;
  case RTMP_MSG_TYPE_ID_USER_CONTROL:
    user_control_message_handler();
    break;
  case RTMP_MSG_TYPE_ID_AUDIO:
  case RTMP_MSG_TYPE_ID_VIDEO:
    av_handler();
    break;
  default:
    YET_LOG_ASSERT(0, "unknown msg type. {}", curr_stream_->header.msg_type_id);
  }
}

void RtmpSession::protocol_control_message_handler() {
  int val;
  AmfOp::decode_int32(curr_stream_->msg->read_pos(), 4, &val, nullptr);

  switch (curr_stream_->header.msg_type_id) {
  case RTMP_MSG_TYPE_ID_SET_CHUNK_SIZE:
    set_chunk_size_handler(val);
    break;
  case RTMP_MSG_TYPE_ID_WIN_ACK_SIZE:
    win_ack_size_handler(val);
    break;
  case RTMP_MSG_TYPE_ID_ABORT:
    YET_LOG_INFO("[{}] recvd protocol control message abort, ignore it. csid:{}", (void *)this, val);
    break;
  case RTMP_MSG_TYPE_ID_ACK:
    YET_LOG_INFO("[{}] recvd protocol control message ack, ignore it. seq num:{}", (void *)this, val);
    break;
  case RTMP_MSG_TYPE_ID_BANDWIDTH:
    YET_LOG_INFO("[{}] recvd protocol control message bandwidth, ignore it. bandwidth:{}", (void *)this, val);
    break;
  default:
    YET_LOG_ASSERT(0, "unknown protocol control message. msg type id:{}", curr_stream_->header.msg_type_id);
  }
}

void RtmpSession::set_chunk_size_handler(int val) {
  peer_chunk_size_ = val;
  YET_LOG_INFO("[{}] ---->Set Chunk Size {}", (void *)this, peer_chunk_size_);
}

void RtmpSession::win_ack_size_handler(int val) {
  peer_win_ack_size_ = val;
  YET_LOG_INFO("[{}] ---->Window Acknowledgement Size {}", (void *)this, peer_win_ack_size_);
}

void RtmpSession::command_message_handler() {
  uint8_t *p = curr_stream_->msg->read_pos();
  char *command_name;
  int command_name_len;
  p = AmfOp::decode_string_with_type(p, curr_stream_->msg->readable_size(), &command_name, &command_name_len, nullptr);

  double transaction_id;
  p = AmfOp::decode_number_with_type(p, curr_stream_->msg->write_pos()-p, &transaction_id, nullptr);

  std::string cmd = std::string(command_name, command_name_len);
  std::size_t left_size = curr_stream_->msg->write_pos()-p;
  if (cmd == "releaseStream" ||
      cmd == "FCPublish" ||
      cmd == "FCUnpublish" ||
      cmd == "FCSubscribe" ||
      cmd == "getStreamLength"
  ) {
    YET_LOG_INFO("[{}] recvd command message {},ignore it.", (void *)this, cmd);

  } else if (cmd == "connect")      { connect_handler(transaction_id, p, left_size);
  } else if (cmd == "createStream") { create_stream_handler(transaction_id, p, left_size);
  } else if (cmd == "publish")      { publish_handler(transaction_id, p, left_size);
  } else if (cmd == "play")         { play_handler(transaction_id, p, left_size);
  } else if (cmd == "deleteStream") { delete_stream_handler(transaction_id, p, left_size);
  } else {
    YET_LOG_ASSERT(0, "Unknown command:{}", std::string(command_name, command_name_len));
  }
}

void RtmpSession::connect_handler(double transaction_id, uint8_t *buf, std::size_t len) {
  YET_LOG_ASSERT(transaction_id == RTMP_TRANSACTION_ID_CONNECT, "invalid transaction_id while rtmp connect. {}", transaction_id)

  AmfObjectItemMap objs;
  buf = AmfOp::decode_object(buf, len, &objs, nullptr);
  YET_LOG_ASSERT(buf, "decode command connect failed.");

  AmfObjectItem *app = objs.get("app");
  YET_LOG_ASSERT(app && app->is_string(), "invalid app field when rtmp connect.");
  app_ = app->get_string();

  YET_LOG_INFO("[{}] ---->connect(\'{}\')", (void *)this, app_);

  // type
  // obs nonprivate

  AmfObjectItem *type = objs.get("type");
  AmfObjectItem *flash_ver = objs.get("flashVer");
  AmfObjectItem *swf_url = objs.get("swfUrl");
  AmfObjectItem *tc_url = objs.get("tcUrl");
  YET_LOG_INFO("[{}] connect app:{}, type:{}, flashVer:{}, swfUrl:{}, tcUrl:{}",
               (void *)this, app_,
               type ? type->stringify() : "",
               flash_ver ? flash_ver->stringify() : "",
               swf_url ? swf_url->stringify() : "",
               tc_url ? tc_url->stringify() : ""
              );

  do_write_win_ack_size();
}

void RtmpSession::do_write_win_ack_size() {
  int wlen = RtmpPackOp::encode_rtmp_msg_win_ack_size_reserve();
  write_buf_.reserve(wlen);
  RtmpPackOp::encode_win_ack_size(write_buf_.write_pos(), RTMP_WINDOW_ACKNOWLEDGEMENT_SIZE);
  YET_LOG_INFO("[{}] <----Window Acknowledgement Size {}", (void *)this, RTMP_WINDOW_ACKNOWLEDGEMENT_SIZE);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        do_write_peer_bandwidth();
      });
}

void RtmpSession::do_write_peer_bandwidth() {
  int wlen = RtmpPackOp::encode_rtmp_msg_peer_bandwidth_reserve();
  write_buf_.reserve(wlen);
  RtmpPackOp::encode_peer_bandwidth(write_buf_.write_pos(), RTMP_PEER_BANDWIDTH);
  YET_LOG_INFO("[{}] <----Set Peer Bandwidth {},Dynamic", (void *)this, RTMP_PEER_BANDWIDTH);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        do_write_chunk_size();
      });
}

void RtmpSession::do_write_chunk_size() {
  int wlen = RtmpPackOp::encode_rtmp_msg_chunk_size_reserve();
  write_buf_.reserve(wlen);
  RtmpPackOp::encode_chunk_size(write_buf_.write_pos(), RTMP_LOCAL_CHUNK_SIZE);
  YET_LOG_INFO("[{}] <----Set Chunk Size {}", (void *)this, RTMP_LOCAL_CHUNK_SIZE);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        do_write_connect_result();
      });
}

void RtmpSession::do_write_connect_result() {
  int wlen = RtmpPackOp::encode_rtmp_msg_connect_result_reserve();
  write_buf_.reserve(wlen);
  RtmpPackOp::encode_connect_result(write_buf_.write_pos());
  YET_LOG_INFO("[{}] <----_result(\'NetConnection.Connect.Success\')", (void *)this);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
      });
}

void RtmpSession::create_stream_handler(double transaction_id, uint8_t *buf, std::size_t len) {
  (void)buf; (void)len;
  create_stream_transaction_id_ = transaction_id;

  // TODO null obj

  YET_LOG_INFO("[{}] ---->createStream()", (void *)this);
  do_write_create_stream_result();
}

void RtmpSession::do_write_create_stream_result() {
  int wlen = RtmpPackOp::encode_rtmp_msg_create_stream_result_reserve();
  write_buf_.reserve(wlen);
  // TODO stream id
  RtmpPackOp::encode_create_stream_result(write_buf_.write_pos(), create_stream_transaction_id_);
  YET_LOG_INFO("[{}] <----_result()", (void *)this);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
      });
}

void RtmpSession::publish_handler(double transaction_id, uint8_t *buf, std::size_t len) {
  std::size_t used_len;
  YET_LOG_ASSERT(transaction_id == RTMP_TRANSACTION_ID_PUBLISH, "invalid transaction_id while rtmp publish. {}", transaction_id)
  buf++; // skip null
  len--;
  char *publishing_name;
  int publishing_name_len;
  buf = AmfOp::decode_string_with_type(buf, len, &publishing_name, &publishing_name_len, &used_len);
  len -= used_len;
  YET_LOG_ASSERT(buf, "invalid publish name field when rtmp publish.");
  char *publishing_type;
  int publishing_type_len;
  buf = AmfOp::decode_string_with_type(buf, len, &publishing_type, &publishing_type_len, &used_len);
  len -= used_len;
  YET_LOG_ASSERT(buf, "invalid publish name field when rtmp publish.");

  live_name_ = std::string(publishing_name, publishing_name_len);
  YET_LOG_INFO("[{}] ---->publish(\'{}\')", (void *)this, live_name_);
  type_ = RTMP_SESSION_TYPE_PUB;
  if (rtmp_publish_cb_) {
    rtmp_publish_cb_(shared_from_this());
  }

  do_write_on_status_publish();
}

void RtmpSession::do_write_on_status_publish() {
  int wlen = RtmpPackOp::encode_rtmp_msg_on_status_publish_reserve();
  write_buf_.reserve(wlen);
  // TODO stream id
  RtmpPackOp::encode_on_status_publish(write_buf_.write_pos(), 1);
  YET_LOG_INFO("[{}] <----onStatus(\'NetStream.Publish.Start\')", (void *)this);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
      });
}

void RtmpSession::play_handler(double transaction_id, uint8_t *buf, std::size_t len) {
  YET_LOG_ASSERT(transaction_id == RTMP_TRANSACTION_ID_PLAY, "invalid transaction id while rtmp play. {}", transaction_id);
  buf++; // skip null
  len--;
  char *name;
  int name_len;
  buf = AmfOp::decode_string_with_type(buf, len, &name, &name_len, nullptr);
  live_name_ = std::string(name, name_len);
  YET_LOG_INFO("[{}] ---->play(\'{}\')", (void *)this, live_name_);

  // TODO
  // start duration reset

  do_write_on_status_play();
}

void RtmpSession::do_write_on_status_play() {
  int wlen = RtmpPackOp::encode_rtmp_msg_on_status_play_reserve();
  write_buf_.reserve(wlen);
  // TODO stream id
  RtmpPackOp::encode_on_status_play(write_buf_.write_pos(), 1);
  YET_LOG_INFO("[{}] <----onStatus(\'NetStream.Play.Start\')", (void *)this);
  auto self(shared_from_this());
  asio::async_write(socket_,
      asio::buffer(write_buf_.read_pos(), wlen),
      [this, self](ErrorCode ec, std::size_t len) {
        SNIPPET_ENTER_CB;
        type_ = RTMP_SESSION_TYPE_SUB;
        if (rtmp_play_cb_) {
          rtmp_play_cb_(shared_from_this());
        }
      });
}

void RtmpSession::delete_stream_handler(double transaction_id, uint8_t *buf, std::size_t len) {
  (void)transaction_id;
  buf++;
  len--;
  double msid;
  AmfOp::decode_number_with_type(buf, len, &msid, nullptr);
  YET_LOG_INFO("[{}] ----->deleteStream({})", (void *)this, msid);
  if (type_ == RTMP_SESSION_TYPE_PUB) {
    if (rtmp_publish_stop_cb_) {
      rtmp_publish_stop_cb_(shared_from_this());
    }
  }
}

void RtmpSession::user_control_message_handler() {
  YET_LOG_ERROR("[{}] TODO", (void *)this);
}

void RtmpSession::data_message_handler() {
  // 7.1.2.

  uint8_t *p = curr_stream_->msg->read_pos();
  auto len = curr_stream_->msg->readable_size();

  char *val;
  int val_len;
  std::size_t used_len;
  p = AmfOp::decode_string_with_type(p, len, &val, &val_len, &used_len);
  YET_LOG_ASSERT(p, "decode metadata failed.");
  if (strncmp(val, "@setDataFrame", val_len) != 0) {
    YET_LOG_ERROR("invalid data message. {}", std::string(val, val_len));
    return;
  }
  len -= used_len;
  uint8_t *meta_pos = p;
  std::size_t meta_size = len;
  p = AmfOp::decode_string_with_type(p, len, &val, &val_len, &used_len);
  YET_LOG_ASSERT(p, "decode metadata failed.");
  if (strncmp(val, "onMetaData", val_len) != 0) {
    YET_LOG_ERROR("invalid data message. {}", std::string(val, val_len));
    return;
  }
  len -= used_len;
  std::shared_ptr<AmfObjectItemMap> metadata = std::make_shared<AmfObjectItemMap>();
  p = AmfOp::decode_ecma_array(p, len, metadata.get(), nullptr);
  YET_LOG_ASSERT(p, "decode metadata failed.");
  YET_LOG_DEBUG("ts:{}, type id:{}, {}", curr_stream_->timestamp_abs, curr_stream_->header.msg_type_id, metadata->stringify());
  if (rtmp_meta_data_cb_) {
    rtmp_meta_data_cb_(shared_from_this(), curr_stream_->msg, meta_pos, meta_size, metadata);
  }
}

void RtmpSession::av_handler() {
  //YET_LOG_DEBUG("[{}] -----recvd {} {}. ts:{} {}, size:{}", (void *this), curr_csid_, curr_stream_->header.msg_type_id, curr_stream_->header.timestamp, curr_stream_->timestamp_abs, curr_stream_->msg->readable_size());

  RtmpHeader h;
  h.csid = curr_stream_->header.msg_type_id == RTMP_MSG_TYPE_ID_AUDIO ? RTMP_CSID_AUDIO : RTMP_CSID_VIDEO;
  h.timestamp = curr_stream_->timestamp_abs;
  h.msg_len = curr_stream_->msg->readable_size();
  h.msg_type_id = curr_stream_->header.msg_type_id;
  h.msg_stream_id = RTMP_MSID;
  if (rtmp_av_data_cb_) {
    rtmp_av_data_cb_(shared_from_this(), curr_stream_->msg, h);
  }
}

void RtmpSession::async_send(BufferPtr buf) {
  bool is_empty = send_buffers_.empty();
  send_buffers_.push(buf);
  if (is_empty) {
    do_send();
  }
}

void RtmpSession::do_send() {
  BufferPtr buf = send_buffers_.front();
  asio::async_write(socket_, asio::buffer(buf->read_pos(), buf->readable_size()),
                    std::bind(&RtmpSession::send_cb, shared_from_this(), _1, _2));
}

void RtmpSession::send_cb(const ErrorCode &ec, std::size_t len) {
  SNIPPET_ENTER_CB;

  send_buffers_.pop();
  if (!send_buffers_.empty()) {
    do_send();
  }
}

void RtmpSession::close() {
  YET_LOG_DEBUG("[{}] close rtmp session.", (void *)this);
  socket_.close();
  if (rtmp_session_close_cb_) {
    rtmp_session_close_cb_(shared_from_this());
  }
}

RtmpStreamPtr RtmpSession::get_or_create_stream(int csid) {
  auto &stream = csid2stream_[csid];
  if (!stream) {
    YET_LOG_DEBUG("[{}] create chunk stream. {}", (void *)this, csid);
    stream = std::make_shared<RtmpStream>();
    stream->msg = std::make_shared<Buffer>(BUF_INIT_LEN_RTMP_COMPLETE_MESSAGE, BUF_SHRINK_LEN_RTMP_COMPLETE_MESSAGE);
  }
  return stream;
}

void RtmpSession::set_rtmp_publish_cb(RtmpEventCb cb) {
  rtmp_publish_cb_ = cb;
}

void RtmpSession::set_rtmp_play_cb(RtmpEventCb cb) {
  rtmp_play_cb_ = cb;
}

void RtmpSession::set_rtmp_publish_stop_cb(RtmpEventCb cb) {
  rtmp_publish_stop_cb_ = cb;
}

void RtmpSession::set_rtmp_session_close_cb(RtmpEventCb cb) {
  rtmp_session_close_cb_ = cb;
}

void RtmpSession::set_rtmp_meta_data_cb(RtmpMetaDataCb cb) {
  rtmp_meta_data_cb_ = cb;
}

void RtmpSession::set_rtmp_av_data_cb(RtmpAvDataCb cb) {
  rtmp_av_data_cb_ = cb;
}

} // namespace yet
