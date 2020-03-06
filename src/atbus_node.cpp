﻿/**
 * @brief 所有channel文件的模式均为 c + channel<br />
 *        使用c的模式是为了简单、结构清晰并且避免异常<br />
 *        附带c++的部分是为了避免命名空间污染并且c++的跨平台适配更加简单
 */

#ifndef _MSC_VER

#include <algorithm>
#include <string>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#else
#pragma comment(lib, "Ws2_32.lib")
#endif

#include <assert.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <std/ref.h>
#include <stdint.h>

#include <common/string_oprs.h>

#include "detail/buffer.h"


#include "atbus_msg_handler.h"
#include "atbus_node.h"

#include "detail/libatbus_protocol.h"
#include "time/time_utility.h"

namespace atbus {
    node::flag_guard_t::flag_guard_t(const node *o, flag_t::type f) : owner(const_cast<node *>(o)), flag(f), holder(false) {
        if (owner && !owner->flags_.test(flag)) {
            holder = true;
            owner->flags_.set(flag, true);
        }
    }


    node::flag_guard_t::~flag_guard_t() {
        if ((*this) && owner) {
            owner->flags_.set(flag, false);
        }
    }

    node::node() : state_(state_t::CREATED), ev_loop_(NULL), static_buffer_(NULL), on_debug(NULL) {
        event_timer_.sec                   = 0;
        event_timer_.usec                  = 0;
        event_timer_.node_sync_push        = 0;
        event_timer_.father_opr_time_point = 0;

        flags_.reset();
    }

    void node::io_stream_channel_del::operator()(channel::io_stream_channel *p) const {
        channel::io_stream_close(p);
        delete p;
    }

    node::~node() {
        if (state_t::CREATED != state_) {
            reset();
        }

        ATBUS_FUNC_NODE_DEBUG(*this, NULL, NULL, NULL, "node destroyed");
    }

    void node::default_conf(conf_t *conf) {
        conf->ev_loop       = NULL;
        conf->children_mask = 0;
        conf->loop_times    = 128;
        conf->ttl           = 16; // 默认最长8次跳转

        conf->first_idle_timeout = ATBUS_MACRO_CONNECTION_CONFIRM_TIMEOUT;
        conf->ping_interval      = 8; // 默认ping包间隔为8s
        conf->retry_interval     = 3;
        conf->fault_tolerant = 2; // 允许最多失败2次，第3次直接失败，默认配置里3次ping包无响应则是最多24s可以发现节点下线
        conf->backlog = ATBUS_MACRO_CONNECTION_BACKLOG;

        conf->msg_size = ATBUS_MACRO_MSG_LIMIT;

        // recv_buffer_size 用于内存/共享内存通道的缓冲区长度，因为本机节点一般数量少所以默认设的大一点
        // default for 128 times of ATBUS_MACRO_MSG_LIMIT = 8MB
        conf->recv_buffer_size = ATBUS_MACRO_MSG_LIMIT * 128;

        // send_buffer_size 用于IO流通道的发送缓冲区长度，远程节点可能数量很多所以设的小一点
        // default for 32 times of ATBUS_MACRO_MSG_LIMIT = 2MB
        conf->send_buffer_size   = ATBUS_MACRO_MSG_LIMIT * 32;
        conf->send_buffer_number = 0; // 默认不使用静态缓冲区，所以设为0

        conf->flags.reset();
    }

    node::ptr_t node::create() {
        ptr_t ret(new node());
        if (!ret) {
            return ret;
        }

        ret->watcher_ = ret;
        return ret;
    }

    int node::init(bus_id_t id, const conf_t *conf) {
        if (state_t::CREATED != state_) {
            reset();
        }

        if (NULL == conf) {
            default_conf(&conf_);
        } else {
            conf_ = *conf;
        }

        ev_loop_ = conf_.ev_loop;
        self_    = endpoint::create(this, id, conf_.children_mask, get_pid(), get_hostname());
        if (!self_) {
            return EN_ATBUS_ERR_MALLOC;
        }
        // 复制配置
        self_->set_flag(endpoint::flag_t::GLOBAL_ROUTER, conf_.flags.test(conf_flag_t::EN_CONF_GLOBAL_ROUTER));

        static_buffer_ = detail::buffer_block::malloc(conf_.msg_size + detail::buffer_block::head_size(conf_.msg_size) +
                                                      16); // 预留hash码32位长度和vint长度);

        self_data_msgs_.clear();
        self_cmd_msgs_.clear();

        state_ = state_t::INITED;
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::start() {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        // 初始化时间
        event_timer_.sec = time(NULL);

        // 连接父节点
        if (!conf_.father_address.empty()) {
            if (!node_father_.node_) {
                // 如果父节点被激活了，那么父节点操作时间必须更新到非0值，以启用这个功能
                if (connect(conf_.father_address.c_str()) >= 0) {
                    event_timer_.father_opr_time_point = event_timer_.sec + conf_.first_idle_timeout;
                    state_                             = state_t::CONNECTING_PARENT;
                } else {
                    event_timer_.father_opr_time_point = event_timer_.sec + conf_.retry_interval;
                    state_                             = state_t::LOST_PARENT;
                }
            }
        } else {
            on_actived();
        }

        return 0;
    }

    int node::reset() {
        // 这个函数可能会在析构时被调用，这时候不能使用watcher_.lock()
        if (flags_.test(flag_t::EN_FT_RESETTING)) {
            return EN_ATBUS_ERR_SUCCESS;
        }
        flags_.set(flag_t::EN_FT_RESETTING, true);
        ATBUS_FUNC_NODE_DEBUG(*this, NULL, NULL, NULL, "node reset");

        // dispatch all self msgs
        {
            while (dispatch_all_self_msgs() > 0)
                ;
        }

        // first save all connection, and then reset it
        typedef detail::auto_select_map<std::string, connection::ptr_t>::type auto_map_t;
        {
            std::vector<auto_map_t::mapped_type> temp_vec;
            temp_vec.reserve(proc_connections_.size());
            for (auto_map_t::iterator iter = proc_connections_.begin(); iter != proc_connections_.end(); ++iter) {
                if (iter->second) {
                    temp_vec.push_back(iter->second);
                }
            }

            // 所有连接断开
            for (size_t i = 0; i < temp_vec.size(); ++i) {
                temp_vec[i]->reset();
            }
        }
        proc_connections_.clear();

        // 销毁endpoint
        if (node_father_.node_) {
            remove_endpoint(node_father_.node_->get_id());
        }
        // endpoint 不应该游离在node以外，所以这里就应该要触发endpoint::reset
        remove_collection(node_brother_);
        remove_collection(node_children_);

        // 清空检测列表和ping列表
        event_timer_.pending_check_list_.clear();
        event_timer_.ping_list.clear();

        // 清空正在连接或握手的列表
        // 必须显式指定断开，以保证会主动断开正在进行的连接
        // 因为正在进行的连接会增加connection的引用计数
        for (evt_timer_t::timer_desc_ls<connection::ptr_t>::type::iterator iter = event_timer_.connecting_list.begin();
             iter != event_timer_.connecting_list.end(); ++iter) {
            iter->second->disconnect();
        }

        event_timer_.connecting_list.clear();

        // 重置自身的endpoint
        if (self_) {
            // 不销毁，下一次替换，保证某些接口可用
            self_->reset();
        }

        // 引用的数据(正在进行的连接)也必须全部释放完成
        // 保证延迟释放的连接也释放完成
        while (!ref_objs_.empty()) {
            uv_run(get_evloop(), UV_RUN_ONCE);
        }

        // 基础数据
        iostream_channel_.reset(); // 这里结束后就不会再触发回调了
        iostream_conf_.reset();

        // 不用重置ev_loop_，其他地方还可以用
        // 只能通过init函数重新初始化来修改它
        // if (NULL != ev_loop_) {
        //     ev_loop_ = NULL;
        // }

        if (NULL != static_buffer_) {
            detail::buffer_block::free(static_buffer_);
            static_buffer_ = NULL;
        }

        conf_.flags.reset();
        state_ = state_t::CREATED;
        flags_.reset();

        self_data_msgs_.clear();
        self_cmd_msgs_.clear();

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::proc(time_t sec, time_t usec) {
        util::time::time_utility::update();
        if (sec > event_timer_.sec) {
            event_timer_.sec  = sec;
            event_timer_.usec = usec;
        } else if (sec == event_timer_.sec && usec > event_timer_.usec) {
            event_timer_.usec = usec;
        }

        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        int ret = 0;
        // stop action happened between previous proc and this one
        if (flags_.test(flag_t::EN_FT_SHUTDOWN)) {
            ret = 1 + dispatch_all_self_msgs();
            reset();
            return ret;
        }

        // TODO 以后可以优化成event_fd通知，这样就不需要轮询了
        // 点对点IO流通道
        for (detail::auto_select_map<std::string, connection::ptr_t>::type::iterator iter = proc_connections_.begin();
             iter != proc_connections_.end(); ++iter) {
            ret += iter->second->proc(*this, sec, usec);
        }

        // connection超时下线
        while (!event_timer_.connecting_list.empty()) {
            evt_timer_t::timer_desc_ls<connection::ptr_t>::pair_type &top = event_timer_.connecting_list.front();
            if (top.first < sec || (top.second && top.second->is_connected())) {
                // 已无效对象则忽略
                if (top.second && false == top.second->is_connected()) {
                    if (event_msg_.on_invalid_connection) {
                        flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
                        event_msg_.on_invalid_connection(std::cref(*this), top.second.get(), EN_ATBUS_ERR_NODE_TIMEOUT);
                    }

                    if (atbus::connection::state_t::DISCONNECTED != top.second->get_status()) {
                        top.second->reset();
                        ATBUS_FUNC_NODE_ERROR(*this, NULL, top.second.get(), EN_ATBUS_ERR_NODE_TIMEOUT, 0);
                    }
                }

                event_timer_.connecting_list.pop_front();
            } else {
                break;
            }
        }

        // 父节点操作
        if (!conf_.father_address.empty() && 0 != event_timer_.father_opr_time_point && event_timer_.father_opr_time_point < sec) {
            // 获取命令节点
            connection *ctl_conn = NULL;
            if (node_father_.node_) {
                ctl_conn = self_->get_ctrl_connection(node_father_.node_.get());
            }

            // 父节点重连
            if (NULL == ctl_conn) {
                int res = connect(conf_.father_address.c_str());
                if (res < 0) {
                    ATBUS_FUNC_NODE_ERROR(*this, NULL, NULL, res, 0);

                    event_timer_.father_opr_time_point = sec + conf_.retry_interval;
                } else {
                    // 下一次判定父节点连接超时再重新连接
                    event_timer_.father_opr_time_point = sec + conf_.first_idle_timeout;
                    state_                             = state_t::CONNECTING_PARENT;
                }
            } else {
                int res = ping_endpoint(*node_father_.node_);
                if (res < 0) {
                    ATBUS_FUNC_NODE_ERROR(*this, NULL, NULL, res, 0);
                }

                // ping包不需要重试
                event_timer_.father_opr_time_point = sec + conf_.ping_interval;
            }
        }

        // Ping包
        while (!event_timer_.ping_list.empty() && event_timer_.ping_list.front().first < sec) {
            endpoint::ptr_t ep = event_timer_.ping_list.front().second.lock();
            event_timer_.ping_list.pop_front();

            // 已移除对象则忽略
            if (ep) {
                // 忽略错误
                ping_endpoint(*ep);

                event_timer_.ping_list.push_back(std::make_pair(sec + conf_.ping_interval, ep));
            }
        }

        // 节点同步协议-推送
        if (0 != event_timer_.node_sync_push && event_timer_.node_sync_push < sec) {
            // 发起子节点同步信息推送
            int res = push_node_sync();
            if (res < 0) {
                event_timer_.node_sync_push = sec + conf_.retry_interval;
            } else {
                event_timer_.node_sync_push = 0;
            }
        }


        // 检测队列
        if (!event_timer_.pending_check_list_.empty()) {
            std::list<endpoint::ptr_t> checked;
            checked.swap(event_timer_.pending_check_list_);

            for (std::list<endpoint::ptr_t>::iterator iter = checked.begin(); iter != checked.end(); ++iter) {
                if (*iter) {
                    if (false == (*iter)->is_available()) {
                        (*iter)->reset();
                        ATBUS_FUNC_NODE_DEBUG(*this, (*iter).get(), NULL, NULL, "endpoint handshake timeout and reset");
                        remove_endpoint((*iter)->get_id());
                    }
                }
            }

            // 再清理一次，因为endpoint::reset可能触发进入pending_check_list_
            event_timer_.pending_check_list_.clear();
        }

        // dispatcher all self msgs
        ret += dispatch_all_self_msgs();

        // stop action happened in any callback
        if (flags_.test(flag_t::EN_FT_SHUTDOWN)) {
            reset();
            return ret + 1;
        }

        return ret;
    }

    int node::poll() {
        // point to point IO stream channels
        int loop_left        = conf_.loop_times;
        size_t stat_dispatch = stat_.dispatch_times;
        while (iostream_channel_ && loop_left > 0 &&
               EN_ATBUS_ERR_EV_RUN == channel::io_stream_run(get_iostream_channel(), adapter::RUN_NOWAIT)) {
            --loop_left;
        }

        return static_cast<int>(stat_.dispatch_times - stat_dispatch);
    }

    int node::listen(const char *addr_str) {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        connection::ptr_t conn = connection::create(this);
        if (!conn) {
            return EN_ATBUS_ERR_MALLOC;
        }

        int ret = conn->listen(addr_str);
        if (ret < 0) {
            return ret;
        }

        // 添加到self_里
        if (false == self_->add_connection(conn.get(), false)) {
            return EN_ATBUS_ERR_ALREADY_INITED;
        }

        // 记录监听地址
        self_->add_listen(conn->get_address().address);

        ATBUS_FUNC_NODE_DEBUG(*this, self_.get(), conn.get(), NULL, "listen to %s, res: %d", addr_str, ret);

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::connect(const char *addr_str) {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        // if there is already connection of this addr not completed, just return success
        for (evt_timer_t::timer_desc_ls<connection::ptr_t>::type::iterator iter = event_timer_.connecting_list.begin();
             iter != event_timer_.connecting_list.end(); ++iter) {
            if (!iter->second || iter->second->is_connected()) {
                continue;
            }

            if (0 == UTIL_STRFUNC_STRNCASE_CMP(addr_str, iter->second->get_address().address.c_str(),
                                               iter->second->get_address().address.size())) {
                return EN_ATBUS_ERR_SUCCESS;
            }
        }

        connection::ptr_t conn = connection::create(this);
        if (!conn) {
            return EN_ATBUS_ERR_MALLOC;
        }

        // 内存通道和共享内存通道不允许协商握手，必须直接指定endpoint
        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem:", addr_str, 4)) {
            return EN_ATBUS_ERR_ACCESS_DENY;
        } else if (0 == UTIL_STRFUNC_STRNCASE_CMP("shm:", addr_str, 4)) {
            return EN_ATBUS_ERR_ACCESS_DENY;
        }

        int ret = conn->connect(addr_str);
        if (ret < 0) {
            return ret;
        }

        ATBUS_FUNC_NODE_DEBUG(*this, NULL, conn.get(), NULL, "connect to %s, res: %d", addr_str, ret);

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::connect(const char *addr_str, endpoint *ep) {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        if (NULL == ep) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // if there is already connection of this addr not completed, just return success
        for (evt_timer_t::timer_desc_ls<connection::ptr_t>::type::iterator iter = event_timer_.connecting_list.begin();
             iter != event_timer_.connecting_list.end(); ++iter) {
            if (!iter->second || iter->second->is_connected()) {
                continue;
            }

            if (iter->second->get_binding() == ep) {
                if (0 == UTIL_STRFUNC_STRNCASE_CMP(addr_str, iter->second->get_address().address.c_str(),
                                                   iter->second->get_address().address.size())) {
                    return EN_ATBUS_ERR_SUCCESS;
                }
            }
        }

        connection::ptr_t conn = connection::create(this);
        if (!conn) {
            return EN_ATBUS_ERR_MALLOC;
        }

        int ret = conn->connect(addr_str);
        if (ret < 0) {
            return ret;
        }

        ATBUS_FUNC_NODE_DEBUG(*this, ep, conn.get(), NULL, "connect to %s and bind to a endpoint, res: %d", addr_str, ret);

        if (0 == UTIL_STRFUNC_STRNCASE_CMP("mem:", addr_str, 4) || 0 == UTIL_STRFUNC_STRNCASE_CMP("shm:", addr_str, 4)) {
            if (ep->add_connection(conn.get(), true)) {
                return EN_ATBUS_ERR_SUCCESS;
            }
        } else {
            if (ep->add_connection(conn.get(), false)) {
                return EN_ATBUS_ERR_SUCCESS;
            }
        }

        return EN_ATBUS_ERR_BAD_DATA;
    }

    int node::disconnect(bus_id_t id) {
        if (node_father_.node_ && id == node_father_.node_->get_id()) {
            endpoint::ptr_t ep_ptr;

            ep_ptr.swap(node_father_.node_);

            // event
            if (event_msg_.on_endpoint_removed) {
                flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
                event_msg_.on_endpoint_removed(std::cref(*this), ep_ptr.get(), EN_ATBUS_ERR_SUCCESS);
            }

            ep_ptr->reset();
            return EN_ATBUS_ERR_SUCCESS;
        }

        endpoint *ep = find_child(node_brother_, id);
        if (NULL != ep && ep->get_id() == id) {
            endpoint::ptr_t ep_ptr = ep->watch();

            // 移除连接关系
            remove_child(node_brother_, id);
            // TODO 移除全局表

            ep_ptr->reset();
            return EN_ATBUS_ERR_SUCCESS;
        }

        ep = find_child(node_children_, id);
        if (NULL != ep && ep->get_id() == id) {
            endpoint::ptr_t ep_ptr = ep->watch();

            // 移除连接关系
            remove_child(node_children_, id);
            // TODO 移除全局表

            ep_ptr->reset();
            return EN_ATBUS_ERR_SUCCESS;
        }

        return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
    }


    int node::send_data(node::bus_id_t tid, int type, const void *buffer, size_t s, bool require_rsp,
                         std::shared_ptr<protocol::custom_route_data> custom_route_data) {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        if (s >= conf_.msg_size) {
            return EN_ATBUS_ERR_BUFF_LIMIT;
        }
        atbus::protocol::msg m;
        if (tid == get_id()) {
            // 发送给自己的数据直接回调数据接口
            m.init(tid, ATBUS_CMD_DATA_TRANSFORM_REQ, type, 0, alloc_msg_seq());

            // fake body
            protocol::forward_data data;
            m.body.forward               = &data;
            m.body.forward->from         = tid;
            m.body.forward->to           = tid;
            m.body.forward->content.ptr  = buffer;
            m.body.forward->content.size = s;
            if (require_rsp) {
                m.body.forward->set_flag(atbus::protocol::forward_data::FLAG_REQUIRE_RSP);
            }

            int ret = send_data_msg(tid, m);

            // remove reference
            m.body.forward = NULL;

            return ret;
        }

        m.init(get_id(), ATBUS_CMD_DATA_TRANSFORM_REQ, type, 0, alloc_msg_seq());

        if (NULL == m.body.make_body(m.body.forward)) {
            return EN_ATBUS_ERR_MALLOC;
        }

        m.body.forward->from         = get_id();
        m.body.forward->to           = tid;
        m.body.forward->content.ptr  = buffer;
        m.body.forward->content.size = s;
        if (require_rsp) {
            m.body.forward->set_flag(atbus::protocol::forward_data::FLAG_REQUIRE_RSP);
        }
        if (custom_route_data){
            m.body.forward->route_data = custom_route_data;
        }

        return send_data_msg(tid, m);
    }


    int node::send_data_msg(bus_id_t tid, atbus::protocol::msg &mb) { return send_data_msg(tid, mb, NULL, NULL); }

    int node::send_data_msg(bus_id_t tid, atbus::protocol::msg &mb, endpoint **ep_out, connection **conn_out) {
        return send_msg(tid, mb, &endpoint::get_data_connection, ep_out, conn_out);
    }

    int node::send_custom_cmd(bus_id_t tid, const void *arr_buf[], size_t arr_size[], size_t arr_count, uint64_t *seq) {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        size_t sum_len = sizeof(atbus::protocol::custom_command_data);
        for (size_t i = 0; i < arr_count; ++i) {
            sum_len += arr_size[i];
        }

        if (sum_len >= conf_.msg_size) {
            return EN_ATBUS_ERR_BUFF_LIMIT;
        }

        atbus::protocol::msg m;
        uint64_t msg_seq = alloc_msg_seq();
        m.init(get_id(), ATBUS_CMD_CUSTOM_CMD_REQ, 0, 0, msg_seq);

        if (NULL == m.body.make_body(m.body.custom)) {
            return EN_ATBUS_ERR_MALLOC;
        }

        m.body.custom->from = get_id();
        m.body.custom->commands.reserve(arr_count);

        for (size_t i = 0; i < arr_count; ++i) {
            atbus::protocol::bin_data_block cmd;
            cmd.ptr  = arr_buf[i];
            cmd.size = arr_size[i];

            m.body.custom->commands.push_back(cmd);
        }

        if (NULL != seq) {
            *seq = msg_seq;
        }

        int ret = send_data_msg(tid, m);
        return ret;
    }

    int node::send_ctrl_msg(bus_id_t tid, atbus::protocol::msg &mb) { return send_ctrl_msg(tid, mb, NULL, NULL); }

    int node::send_ctrl_msg(bus_id_t tid, atbus::protocol::msg &mb, endpoint **ep_out, connection **conn_out) {
        return send_msg(tid, mb, &endpoint::get_ctrl_connection, ep_out, conn_out);
    }

    int node::send_msg(bus_id_t tid, atbus::protocol::msg &m, endpoint::get_connection_fn_t fn, endpoint **ep_out, connection **conn_out) {
        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        if (tid == get_id()) {
            if (!((ATBUS_CMD_DATA_TRANSFORM_REQ == m.head.cmd && m.body.forward) ||
                  (ATBUS_CMD_CUSTOM_CMD_REQ == m.head.cmd && m.body.custom))) {
                ATBUS_FUNC_NODE_ERROR(*this, get_self_endpoint(), NULL, EN_ATBUS_ERR_ATNODE_INVALID_ID, 0);
            }

            assert((ATBUS_CMD_DATA_TRANSFORM_REQ == m.head.cmd && m.body.forward) ||
                   ((ATBUS_CMD_CUSTOM_CMD_REQ == m.head.cmd || ATBUS_CMD_CUSTOM_CMD_RSP == m.head.cmd) && m.body.custom));
            typedef std::vector<unsigned char> bin_data_block_t;

            const size_t msg_head_len = sizeof(::atbus::protocol::msg_head);
            // self data msg
            if (ATBUS_CMD_DATA_TRANSFORM_REQ == m.head.cmd && m.body.forward) {
                self_data_msgs_.push_back(bin_data_block_t());
                bin_data_block_t &bin_data = self_data_msgs_.back();
                bin_data.resize(sizeof(int) + msg_head_len + m.body.forward->content.size);
                memcpy(&bin_data[0], &m.head, msg_head_len);
                memcpy(&bin_data[msg_head_len], &m.body.forward->flags, sizeof(int));
                memcpy(&bin_data[sizeof(int) + msg_head_len], m.body.forward->content.ptr, m.body.forward->content.size);
            }

            // self command msg
            if ((ATBUS_CMD_CUSTOM_CMD_REQ == m.head.cmd || ATBUS_CMD_CUSTOM_CMD_RSP == m.head.cmd) && m.body.custom) {
                self_cmd_msgs_.push_back(std::vector<bin_data_block_t>());
                std::vector<bin_data_block_t> &cmds = self_cmd_msgs_.back();
                cmds.reserve(1 + m.body.custom->commands.size());
                cmds.push_back(bin_data_block_t());
                bin_data_block_t &head_data = cmds.back();
                head_data.resize(msg_head_len);
                memcpy(&head_data[0], &m.head, msg_head_len);

                for (size_t i = 0; i < m.body.custom->commands.size(); ++i) {
                    const ::atbus::protocol::bin_data_block &src_cmd_param = m.body.custom->commands[i];
                    cmds.push_back(bin_data_block_t());
                    bin_data_block_t &bin_data = cmds.back();
                    bin_data.resize(src_cmd_param.size);
                    memcpy(&bin_data[0], src_cmd_param.ptr, src_cmd_param.size);
                }
            }

            dispatch_all_self_msgs();
            return EN_ATBUS_ERR_SUCCESS;
        }

        connection *conn = NULL;
        int res          = get_remote_channel(tid, fn, ep_out, &conn);
        if (NULL != conn_out) {
            *conn_out = conn;
        }

        if (res < 0) {
            return res;
        }

        if (NULL == conn) {
            return EN_ATBUS_ERR_ATNODE_NO_CONNECTION;
        }

        if (NULL != m.body.forward) {
            m.body.forward->router.push_back(get_id());
        }

        // head 里永远是发起方bus_id
        m.head.src_bus_id = get_id();

        return msg_handler::send_msg(*this, *conn, m);
    }

    int node::get_remote_channel(bus_id_t tid, endpoint::get_connection_fn_t fn, endpoint **ep_out, connection **conn_out) {
#define ASSIGN_EPCONN()                   \
    if (NULL != ep_out) *ep_out = target; \
    if (NULL != conn_out) *conn_out = conn

        endpoint *target = NULL;
        connection *conn = NULL;

        ASSIGN_EPCONN();

        if (state_t::CREATED == state_) {
            return EN_ATBUS_ERR_NOT_INITED;
        }

        if (tid == get_id()) {
            return EN_ATBUS_ERR_ATNODE_INVALID_ID;
        }

        do {

            // tid 为0直接发给父节点
            if (tid == 0){
                if (node_father_.node_){
                    target = node_father_.node_.get();
                    conn   = (self_.get()->*fn)(target);

                    ASSIGN_EPCONN();
                    break;
                }else{
                    return EN_ATBUS_ERR_ATNODE_INVALID_ID;
                }
            }

            // 父节点单独判定，防止父节点被判定为兄弟节点
            if (node_father_.node_ && is_parent_node(tid)) {
                target = node_father_.node_.get();
                conn   = (self_.get()->*fn)(target);

                ASSIGN_EPCONN();
                break;
            }

            // 兄弟节点(父节点会被判为可能是兄弟节点)
            if (is_brother_node(tid)) {
                target = find_child(node_brother_, tid);
                if (NULL != target && target->is_child_node(tid)) {
                    conn = (self_.get()->*fn)(target);

                    ASSIGN_EPCONN();
                    break;
                } else if (false == get_self_endpoint()->get_flag(endpoint::flag_t::GLOBAL_ROUTER) && node_father_.node_) {
                    // 如果没有全量表则发给父节点
                    /*
                    //       F1
                    //      /  \
                    //    C11  C12
                    // 当C11发往C12时触发这种情况
                    */
                    target = node_father_.node_.get();
                    conn   = (self_.get()->*fn)(target);

                    ASSIGN_EPCONN();
                    break;
                }

                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }

            // 子节点
            if (is_child_node(tid)) {
                target = find_child(node_children_, tid);
                if (NULL != target && target->is_child_node(tid)) {
                    conn = (self_.get()->*fn)(target);

                    ASSIGN_EPCONN();
                    break;
                }
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }

            // 其他情况,如果没有全量表则发给父节点
            /*
            //       F1 ------------ F2
            //      /  \            /  \
            //    C11  C12        C21  C22
            // 当C11发往C21或C22时触发这种情况
            */
            if (node_father_.node_ && false == get_self_endpoint()->get_flag(endpoint::flag_t::GLOBAL_ROUTER)) {
                target = node_father_.node_.get();
                conn   = (self_.get()->*fn)(target);

                ASSIGN_EPCONN();
                break;
            }
        } while (false);

#undef ASSIGN_EPCONN

        if (NULL == conn || NULL == target) {
            return EN_ATBUS_ERR_ATNODE_NO_CONNECTION;
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    endpoint *node::get_endpoint(bus_id_t tid) {
        if (is_parent_node(tid)) {
            return node_father_.node_.get();
        }

        // 直连兄弟节点
        if (0 == get_id() || is_brother_node(tid)) {
            endpoint *res = find_child(node_brother_, tid);
            if (NULL != res && res->get_id() == tid) {
                return res;
            }
            return NULL;
        }

        // 直连子节点
        if (is_child_node(tid)) {
            endpoint *res = find_child(node_children_, tid);
            if (NULL != res && res->get_id() == tid) {
                return res;
            }
            return NULL;
        }

        return NULL;
    }

    const endpoint *node::get_endpoint(bus_id_t tid) const { return const_cast<node *>(this)->get_endpoint(tid); }

    int node::add_endpoint(endpoint::ptr_t ep) {
        if (!ep) {
            return EN_ATBUS_ERR_PARAMS;
        }

        if (this != ep->get_owner()) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // 父节点单独判定，由于防止测试兄弟节点
        if (0 != get_id() && ep->get_children_mask() > self_->get_children_mask() && ep->is_child_node(get_id())) {
            if (!node_father_.node_) {
                node_father_.node_ = ep;

                add_ping_timer(ep);

                if ((state_t::LOST_PARENT == get_state() || state_t::CONNECTING_PARENT == get_state()) &&
                    check_flag(flag_t::EN_FT_PARENT_REG_DONE)) {
                    on_actived();
                }

                // event
                if (event_msg_.on_endpoint_added) {
                    flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
                    event_msg_.on_endpoint_added(std::cref(*this), ep.get(), EN_ATBUS_ERR_SUCCESS);
                }

                return EN_ATBUS_ERR_SUCCESS;
            } else {
                // 父节点只能有一个
                return EN_ATBUS_ERR_ATNODE_INVALID_ID;
            }
        }

        // 兄弟节点(父节点会被判为可能是兄弟节点)
        if (0 == get_id() || is_brother_node(ep->get_id())) {
            // event will be triggered in insert_child()
            if (insert_child(node_brother_, ep)) {
                add_ping_timer(ep);

                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_MASK_CONFLICT;
            }
        }

        // 子节点
        if (is_child_node(ep->get_id())) {
            // event will be triggered in insert_child()
            if (insert_child(node_children_, ep)) {
                add_ping_timer(ep);

                // TODO 子节点上线上报
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_MASK_CONFLICT;
            }
        }

        return EN_ATBUS_ERR_ATNODE_INVALID_ID;
    }

    int node::remove_endpoint(bus_id_t tid) {
        // 父节点单独判定，由于防止测试兄弟节点
        if (is_parent_node(tid)) {
            endpoint::ptr_t ep = node_father_.node_;

            node_father_.node_.reset();
            state_ = state_t::LOST_PARENT;

            // set reconnect to father into retry interval
            event_timer_.father_opr_time_point = get_timer_sec() + conf_.retry_interval;

            // event
            if (event_msg_.on_endpoint_removed) {
                flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
                event_msg_.on_endpoint_removed(std::cref(*this), ep.get(), EN_ATBUS_ERR_SUCCESS);
            }

            // if not activited, shutdown
            if (!flags_.test(flag_t::EN_FT_ACTIVED)) {
                ATBUS_FUNC_NODE_FATAL_SHUTDOWN(*this, ep.get(), NULL, EN_ATBUS_ERR_ATNODE_MASK_CONFLICT, 0);
            }
            return EN_ATBUS_ERR_SUCCESS;
        }

        // 兄弟节点(父节点会被判为可能是兄弟节点)
        if (0 == get_id() || is_brother_node(tid)) {
            // event will be triggered in remove_child()
            if (remove_child(node_brother_, tid)) {
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
            }
        }

        // 子节点
        if (is_child_node(tid)) {
            // event will be triggered in remove_child()
            if (remove_child(node_children_, tid)) {
                // TODO 子节点下线上报
                return EN_ATBUS_ERR_SUCCESS;
            } else {
                return EN_ATBUS_ERR_ATNODE_NOT_FOUND;
            }
        }

        return EN_ATBUS_ERR_ATNODE_INVALID_ID;
    }

    bool node::is_endpoint_available(bus_id_t tid) const {
        if (!flags_.test(flag_t::EN_FT_ACTIVED)) {
            return false;
        }

        if (!self_) {
            return false;
        }

        endpoint *ep = const_cast<endpoint *>(get_endpoint(tid));
        if (NULL == ep) {
            return false;
        }

        return NULL != self_->get_data_connection(ep, false);
    }

    adapter::loop_t *node::get_evloop() {
        // if just created, do not alloc new event loop
        if (state_t::CREATED == state_) {
            return ev_loop_;
        }

        if (NULL != ev_loop_) {
            return ev_loop_;
        }

        if (NULL != conf_.ev_loop) {
            return ev_loop_ = conf_.ev_loop;
        }

        ev_loop_ = uv_default_loop();
        return ev_loop_;
    }

    bool node::is_child_node(bus_id_t id) const {
        if (0 == get_id()) {
            return false;
        }
        return self_->is_child_node(id);
    }

    bool node::is_brother_node(bus_id_t id) const {
        if (0 == get_id()) {
            return true;
        }

        if (node_father_.node_) {
            return self_->is_brother_node(id, node_father_.node_->get_children_mask());
        } else {
            return self_->is_brother_node(id, 0);
        }
    }

    bool node::is_parent_node(bus_id_t id) const {
        if (0 == get_id()) {
            return false;
        }

        if (node_father_.node_) {
            return self_->is_parent_node(id, node_father_.node_->get_id(), node_father_.node_->get_children_mask());
        }

        return false;
    }

    int node::get_pid() {
#ifdef _MSC_VER
        return _getpid();
#else
        return getpid();
#endif
    }

    static std::string &host_name_buffer() {
        static std::string server_addr;
        return server_addr;
    }

    const std::string &node::get_hostname() {
        std::string &hn = host_name_buffer();
        if (!hn.empty()) {
            return hn;
        }

        // use sorted mac address first, hostname is too easy to conflict
        {
            std::vector<std::string> all_outter_inters;
            uv_interface_address_t *interface_addrs = NULL;
            int interface_sz                        = 0;
            size_t total_size                       = 0;
            uv_interface_addresses(&interface_addrs, &interface_sz);
            for (int i = 0; i < interface_sz; ++i) {
                uv_interface_address_t *inter_addr = interface_addrs + i;
                if (inter_addr->is_internal) {
                    continue;
                }

                std::string one_addr;
                size_t dump_index = 0;
                while (dump_index < sizeof(inter_addr->phys_addr) && 0 == inter_addr->phys_addr[dump_index]) {
                    ++dump_index;
                }
                if (dump_index < sizeof(inter_addr->phys_addr)) {
                    one_addr.resize((sizeof(inter_addr->phys_addr) - dump_index) * 2);
                    util::string::dumphex(inter_addr->phys_addr + dump_index, (sizeof(inter_addr->phys_addr) - dump_index), &one_addr[0]);
                }

                if (!one_addr.empty()) {
                    all_outter_inters.push_back(one_addr);
                    total_size += one_addr.size();
                }
            }

            if (total_size > 0) {
                hn.reserve(total_size + all_outter_inters.size());
            }

            std::sort(all_outter_inters.begin(), all_outter_inters.end());
            std::vector<std::string>::iterator new_end = std::unique(all_outter_inters.begin(), all_outter_inters.end());
            all_outter_inters.erase(new_end, all_outter_inters.end());
            for (size_t i = 0; i < all_outter_inters.size(); ++i) {
                if (i > 0) {
                    hn += ":";
                }
                hn += all_outter_inters[i];
            }

            if (NULL != interface_addrs) {
                uv_free_interface_addresses(interface_addrs, interface_sz);
            }
        }

        if (!hn.empty()) {
            return hn;
        }

        // @see man gethostname
        // 255 or less in posix standard
        // 64 in linux(defined as HOST_NAME_MAX)
        // 256 or less in windows(https://msdn.microsoft.com/en-us/library/windows/desktop/ms738527(v=vs.85).aspx)
        char buffer[256] = {0};
        if (0 == gethostname(buffer, sizeof(buffer))) {
            hn = buffer;
        }
#ifdef _MSC_VER
        else {
            if (WSANOTINITIALISED == WSAGetLastError()) {
                WSADATA wsaData;
                WORD version = MAKEWORD(2, 0);
                if (0 == WSAStartup(version, &wsaData) && 0 == gethostname(buffer, sizeof(buffer))) {
                    hn = buffer;
                }
            }
        }
#endif

        return hn;
    }

    bool node::set_hostname(const std::string &hn, bool force) {
        std::string &h = host_name_buffer();
        if (force || h.empty()) {
            h = hn;
            return true;
        }

        return false;
    }

    bool node::add_proc_connection(connection::ptr_t conn) {
        if (state_t::CREATED == state_) {
            return false;
        }

        if (!conn || conn->get_address().address.empty() ||
            proc_connections_.end() != proc_connections_.find(conn->get_address().address)) {
            return false;
        }

        proc_connections_[conn->get_address().address] = conn;
        return true;
    }

    bool node::remove_proc_connection(const std::string &conn_key) {
        detail::auto_select_map<std::string, connection::ptr_t>::type::iterator iter = proc_connections_.find(conn_key);
        if (iter == proc_connections_.end()) {
            return false;
        }

        proc_connections_.erase(iter);
        return true;
    }

    bool node::add_connection_timer(connection::ptr_t conn) {
        if (state_t::CREATED == state_) {
            return false;
        }

        if (!conn) {
            return false;
        }

        // 如果处于握手阶段，发送节点关系逻辑并加入握手连接池并加入超时判定池
        if (false == conn->is_connected()) {
            event_timer_.connecting_list.push_back(std::make_pair(event_timer_.sec + conf_.first_idle_timeout, conn));
        }
        return true;
    }

    time_t node::get_timer_sec() const { return event_timer_.sec; }

    time_t node::get_timer_usec() const { return event_timer_.usec; }

    void node::on_recv(connection *conn, protocol::msg *m, int status, int errcode) {
        if (status < 0 || errcode < 0) {
            ATBUS_FUNC_NODE_ERROR(*this, NULL, conn, status, errcode);

            if (NULL != conn) {
                endpoint *ep = conn->get_binding();
                if (NULL != ep) {
                    add_endpoint_fault(*ep);
                }
            }
            return;
        }

        // 内部协议处理
        int res = msg_handler::dispatch_msg(*this, conn, m, status, errcode);
        if (res < 0) {
            if (NULL != conn) {
                endpoint *ep = conn->get_binding();
                if (NULL != ep) {
                    add_endpoint_fault(*ep);
                }
            }

            return;
        }

        if (NULL == m) {
            return;
        }

        if (NULL != conn) {
            endpoint *ep = conn->get_binding();
            if (NULL != ep) {
                // 如果消息为回发转发消息失败，并且回发成功，则是为正确流程，不能标记错误
                if (m->head.cmd != ATBUS_CMD_DATA_TRANSFORM_RSP && m->head.ret < 0) {
                    add_endpoint_fault(*ep);
                } else {
                    ep->clear_stat_fault();
                }
            }
        }
    }

    void node::on_recv_data(const endpoint *ep, connection *conn, const protocol::msg &m, const void *buffer, size_t s) const {
        if (NULL == ep && NULL != conn) {
            ep = conn->get_binding();
        }

        if (event_msg_.on_recv_msg) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_recv_msg(std::cref(*this), ep, conn, std::cref(m), buffer, s);
        }
    }

    void node::on_send_data_failed(const endpoint *ep, const connection *conn, const protocol::msg *m) {
        if (event_msg_.on_send_data_failed) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_send_data_failed(std::cref(*this), ep, conn, m);
        }
    }

    int node::on_error(const char * /*file_path*/, size_t /*line*/, const endpoint *ep, const connection *conn, int status, int errcode) {
        if (NULL == ep && NULL != conn) {
            ep = conn->get_binding();
        }

        if (event_msg_.on_error) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_error(std::cref(*this), ep, conn, status, errcode);
        }

        return status;
    }

    int node::on_disconnect(const connection *conn) {
        if (NULL == conn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // 父节点断线逻辑则重置状态
        if (state_t::CONNECTING_PARENT == state_ && !conf_.father_address.empty() && conf_.father_address == conn->get_address().address) {
            state_ = state_t::LOST_PARENT;

            // set reconnect to father into retry interval
            event_timer_.father_opr_time_point = get_timer_sec() + conf_.retry_interval;

            // if not activited, shutdown
            if (!flags_.test(flag_t::EN_FT_ACTIVED)) {
                // lost conflict response from the parent, maybe cancled.
                ATBUS_FUNC_NODE_FATAL_SHUTDOWN(*this, NULL, conn, EN_ATBUS_ERR_ATNODE_MASK_CONFLICT, UV_ECANCELED);
            }
        }
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_new_connection(connection *conn) {
        if (NULL == conn) {
            return EN_ATBUS_ERR_PARAMS;
        }

        // 如果ID有效，且是IO流连接，则发送注册协议
        // ID为0则是临时节点，不需要注册
        if (get_id() && conn->check_flag(connection::flag_t::REG_FD) && false == conn->check_flag(connection::flag_t::LISTEN_FD)) {
            int ret = msg_handler::send_reg(ATBUS_CMD_NODE_REG_REQ, *this, *conn, 0, 0);
            if (ret < 0) {
                ATBUS_FUNC_NODE_ERROR(*this, NULL, conn, ret, 0);
                conn->reset();
                return ret;
            }
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_shutdown(int reason) {

        if (!flags_.test(flag_t::EN_FT_ACTIVED)) {
            return EN_ATBUS_ERR_SUCCESS;
        }
        // flags_.set(flag_t::EN_FT_ACTIVED, false); // will be reset in reset()

        if (event_msg_.on_node_down) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_node_down(std::cref(*this), reason);
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_reg(const endpoint *ep, const connection *conn, int status) {
        if (event_msg_.on_reg) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_reg(std::cref(*this), ep, conn, status);
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_actived() {
        state_ = state_t::RUNNING;

        if (flags_.test(flag_t::EN_FT_ACTIVED)) {
            return EN_ATBUS_ERR_SUCCESS;
        }

        flags_.set(flag_t::EN_FT_ACTIVED, true);
        if (event_msg_.on_node_up) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_node_up(std::cref(*this), EN_ATBUS_ERR_SUCCESS);
        }

        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_parent_reg_done() {
        flags_.set(flag_t::EN_FT_PARENT_REG_DONE, true);
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_custom_cmd(const endpoint *ep, const connection *conn, bus_id_t from,
                            const std::vector<std::pair<const void *, size_t> > &cmd_args, std::list<std::string> &rsp) {
        if (event_msg_.on_custom_cmd) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_custom_cmd(std::cref(*this), ep, conn, from, std::cref(cmd_args), std::ref(rsp));
        }
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::on_custom_rsp(const endpoint *ep, const connection *conn, bus_id_t from,
                            const std::vector<std::pair<const void *, size_t> > &cmd_args, uint64_t seq) {
        if (event_msg_.on_custom_rsp) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_custom_rsp(std::cref(*this), ep, conn, from, std::cref(cmd_args), seq);
        }
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::shutdown(int reason) {
        if (flags_.test(flag_t::EN_FT_SHUTDOWN)) {
            return 0;
        }

        flags_.set(flag_t::EN_FT_SHUTDOWN, true);
        return on_shutdown(reason);
    }

    int node::fatal_shutdown(const char *file_path, size_t line, const endpoint *ep, const connection *conn, int status, int errcode) {
        if (flags_.test(flag_t::EN_FT_SHUTDOWN)) {
            return 0;
        }

        shutdown(status);
        on_error(file_path, line, ep, conn, status, errcode);
        return 0;
    }

    int node::dispatch_all_self_msgs() {
        int ret = 0;

        // recursive call will be ignored
        if (check_flag(flag_t::EN_FT_RECV_SELF_MSG) || check_flag(flag_t::EN_FT_IN_CALLBACK)) {
            return ret;
        }
        flag_guard_t fgd(this, flag_t::EN_FT_RECV_SELF_MSG);
        int loop_left = conf_.loop_times;
        if (loop_left <= 0) {
            loop_left = 10240;
        }

        const size_t msg_head_len = sizeof(::atbus::protocol::msg_head);

        typedef std::vector<unsigned char> bin_data_block_t;
        while (loop_left-- > 0 && !self_data_msgs_.empty()) {
            bin_data_block_t &bin_data = self_data_msgs_.front();
            atbus::protocol::msg m;
            // copy head
            memcpy(&m.head, &bin_data[0], msg_head_len);

            // fake body
            protocol::forward_data data;
            m.body.forward               = &data;
            m.body.forward->from         = get_id();
            m.body.forward->to           = get_id();
            m.body.forward->content.ptr  = &bin_data[sizeof(int) + msg_head_len];
            m.body.forward->content.size = bin_data.size() - sizeof(int) - msg_head_len;
            memcpy(&m.body.forward->flags, &bin_data[msg_head_len], sizeof(int));

            on_recv_data(get_self_endpoint(), NULL, m, m.body.forward->content.ptr, m.body.forward->content.size);
            ++ret;

            // fake response
            if (NULL != m.body.forward && m.body.forward->check_flag(atbus::protocol::forward_data::FLAG_REQUIRE_RSP)) {
                m.init(get_id(), ATBUS_CMD_DATA_TRANSFORM_RSP, m.head.type, 0, m.head.sequence);
                on_send_data_failed(get_self_endpoint(), NULL, &m);
            }

            // remove reference
            m.body.forward = NULL;

            // pop front msg
            self_data_msgs_.pop_front();
        }

        while (loop_left-- > 0 && !self_cmd_msgs_.empty()) {
            std::vector<bin_data_block_t> &bin_datas = self_cmd_msgs_.front();
            atbus::protocol::msg m;
            // fake body
            protocol::custom_command_data data;
            m.body.custom       = &data;
            m.body.custom->from = get_id();

            do {
                if (bin_datas.empty() || msg_head_len != bin_datas[0].size()) {
                    assert(!bin_datas.empty() && msg_head_len == bin_datas[0].size());
                    break;
                }
                // copy head
                memcpy(&m.head, &bin_datas[0][0], msg_head_len);
                m.body.custom->commands.resize(bin_datas.size() - 1);
                for (size_t i = 1; i < bin_datas.size(); ++i) {
                    m.body.custom->commands[i - 1].ptr  = &bin_datas[i][0];
                    m.body.custom->commands[i - 1].size = bin_datas[i].size();
                }

                on_recv(NULL, &m, 0, 0);
                ++ret;

            } while (false);

            // remove reference
            m.body.custom = NULL;

            // pop front msg
            self_cmd_msgs_.pop_front();
        }
        return ret;
    }

    int node::ping_endpoint(endpoint &ep) {
        // 检测上一次ping是否返回
        if (0 != ep.get_stat_ping()) {
            if (add_endpoint_fault(ep)) {
                return EN_ATBUS_ERR_ATNODE_FAULT_TOLERANT;
            }
        }


        // 允许跳过未连接或握手完成的endpoint
        connection *ctl_conn = self_->get_ctrl_connection(&ep);
        if (NULL == ctl_conn) {
            add_endpoint_fault(ep);
            return EN_ATBUS_ERR_SUCCESS;
        }

        // 出错则增加错误计数
        uint32_t ping_seq = msg_seq_alloc_.inc();
        int res           = msg_handler::send_ping(*this, *ctl_conn, ping_seq);
        if (res < 0) {
            add_endpoint_fault(ep);
            return res;
        }

        // no data channel is also a error
        if (NULL == self_->get_data_connection(&ep, false)) {
            add_endpoint_fault(ep);
        }

        ep.set_stat_ping(ping_seq);
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::push_node_sync() {
        // TODO 防止短时间内批量上报注册协议，所以合并上报数据包

        // TODO 给所有需要全局路由表的子节点下发数据
        return EN_ATBUS_ERR_SUCCESS;
    }

    int node::pull_node_sync() {
        // TODO 拉取全局节点信息表
        return EN_ATBUS_ERR_SUCCESS;
    }

    uint64_t node::alloc_msg_seq() {
        uint64_t ret = 0;
        while (!ret) {
            ret = msg_seq_alloc_.inc();
        }
        return ret;
    }

    void node::add_check_list(const endpoint::ptr_t &ep) {
        // 重置过程中不需要再加进来了，反正等会也会移除
        // 这个代码加不加一样，只不过会少一些废操作
        if (flags_.test(flag_t::EN_FT_RESETTING)) {
            return;
        }

        if (ep) {
            event_timer_.pending_check_list_.push_back(ep);
        }
    }

    void node::set_on_recv_handle(evt_msg_t::on_recv_msg_fn_t fn) { event_msg_.on_recv_msg = fn; }
    const node::evt_msg_t::on_recv_msg_fn_t &node::get_on_recv_handle() const { return event_msg_.on_recv_msg; }

    void node::set_on_custom_route_handle(evt_msg_t::on_custom_route_fn_t fn) { event_msg_.on_custom_route = fn; }
    const node::evt_msg_t::on_custom_route_fn_t &node::get_on_custom_route_handle() const { return event_msg_.on_custom_route; }

    void node::set_on_send_data_failed_handle(evt_msg_t::on_send_data_failed_fn_t fn) { event_msg_.on_send_data_failed = fn; }
    const node::evt_msg_t::on_send_data_failed_fn_t &node::get_on_send_data_failed_handle() const { return event_msg_.on_send_data_failed; }

    void node::set_on_error_handle(node::evt_msg_t::on_error_fn_t fn) { event_msg_.on_error = fn; }
    const node::evt_msg_t::on_error_fn_t &node::get_on_error_handle() const { return event_msg_.on_error; }

    void node::set_on_register_handle(node::evt_msg_t::on_reg_fn_t fn) { event_msg_.on_reg = fn; }
    const node::evt_msg_t::on_reg_fn_t &node::get_on_register_handle() const { return event_msg_.on_reg; }

    void node::set_on_shutdown_handle(evt_msg_t::on_node_down_fn_t fn) { event_msg_.on_node_down = fn; }
    const node::evt_msg_t::on_node_down_fn_t &node::get_on_shutdown_handle() const { return event_msg_.on_node_down; }

    void node::set_on_available_handle(node::evt_msg_t::on_node_up_fn_t fn) { event_msg_.on_node_up = fn; }
    const node::evt_msg_t::on_node_up_fn_t &node::get_on_available_handle() const { return event_msg_.on_node_up; }

    void node::set_on_invalid_connection_handle(node::evt_msg_t::on_invalid_connection_fn_t fn) { event_msg_.on_invalid_connection = fn; }
    const node::evt_msg_t::on_invalid_connection_fn_t &node::get_on_invalid_connection_handle() const {
        return event_msg_.on_invalid_connection;
    }

    void node::set_on_custom_cmd_handle(evt_msg_t::on_custom_cmd_fn_t fn) { event_msg_.on_custom_cmd = fn; }
    const node::evt_msg_t::on_custom_cmd_fn_t &node::get_on_custom_cmd_handle() const { return event_msg_.on_custom_cmd; }

    void node::set_on_custom_rsp_handle(evt_msg_t::on_custom_rsp_fn_t fn) { event_msg_.on_custom_rsp = fn; }
    const node::evt_msg_t::on_custom_rsp_fn_t &node::get_on_custom_rsp_handle() const { return event_msg_.on_custom_rsp; }

    void node::set_on_add_endpoint_handle(evt_msg_t::on_add_endpoint_fn_t fn) { event_msg_.on_endpoint_added = fn; }
    const node::evt_msg_t::on_add_endpoint_fn_t &node::get_on_add_endpoint_handle() const { return event_msg_.on_endpoint_added; }

    void node::set_on_remove_endpoint_handle(evt_msg_t::on_remove_endpoint_fn_t fn) { event_msg_.on_endpoint_removed = fn; }
    const node::evt_msg_t::on_remove_endpoint_fn_t &node::get_on_remove_endpoint_handle() const { return event_msg_.on_endpoint_removed; }

    void node::ref_object(void *obj) {
        if (NULL == obj) {
            return;
        }

        ref_objs_.insert(obj);
    }

    void node::unref_object(void *obj) { ref_objs_.erase(obj); }

    endpoint *node::find_child(endpoint_collection_t &coll, bus_id_t id) {
        // key 保存为子域上界，所以第一个查找的节点要么直接是value，要么是value的子节点
        endpoint_collection_t::iterator iter = coll.lower_bound(id);
        if (iter == coll.end()) {
            return NULL;
        }

        if (iter->second->get_id() == id) {
            return iter->second.get();
        }

        // 不能直接发送到间接子节点，所以直接发给直接子节点由其转发即可
        if (iter->second->is_child_node(id)) {
            return iter->second.get();
        }

        return NULL;
    }

    bool node::insert_child(endpoint_collection_t &coll, endpoint::ptr_t ep) {
        if (!ep) {
            return false;
        }

        bus_id_t maskv = endpoint::get_children_max_id(ep->get_id(), ep->get_children_mask());

        // key 保存为子域上界，所以第一个查找的节点要么目标节点的父节点，要么子节点域交叉
        endpoint_collection_t::iterator iter = coll.lower_bound(ep->get_id());

        // 如果在所有子节点域外则直接添加
        if (iter == coll.end()) {
            // 有可能这个节点覆盖最后一个
            if (!coll.empty() && ep->is_child_node(coll.rbegin()->second->get_id())) {
                return false;
            }

            coll[maskv] = ep;

            // event
            if (event_msg_.on_endpoint_added) {
                flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
                event_msg_.on_endpoint_added(std::cref(*this), ep.get(), EN_ATBUS_ERR_SUCCESS);
            }
            return true;
        }

        // 如果是数据merge则出错退出
        if (iter->second->get_id() == ep->get_id()) {
            return false;
        }

        // 如果新节点是老节点的子节点，则失败退出
        if (iter->second->is_child_node(ep->get_id()) || ep->is_child_node(iter->second->get_id())) {
            return false;
        }

        // 如果有老节点是新节点的子节点，则失败退出
        if (iter != coll.begin()) {
            --iter;
            if (iter != coll.end() && ep->is_child_node(iter->second->get_id())) {
                return false;
            }
        }

        coll[maskv] = ep;

        // event
        if (event_msg_.on_endpoint_added) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_endpoint_added(std::cref(*this), ep.get(), EN_ATBUS_ERR_SUCCESS);
        }
        return true;
    }

    bool node::remove_child(endpoint_collection_t &coll, bus_id_t id) {
        endpoint_collection_t::iterator iter = coll.lower_bound(id);
        if (iter == coll.end()) {
            return false;
        }

        if (iter->second->get_id() != id) {
            return false;
        }

        endpoint::ptr_t ep = iter->second;
        coll.erase(iter);

        // event
        if (event_msg_.on_endpoint_removed) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            event_msg_.on_endpoint_removed(std::cref(*this), ep.get(), EN_ATBUS_ERR_SUCCESS);
        }
        return true;
    }

    bool node::remove_collection(endpoint_collection_t &coll) {
        endpoint_collection_t ec;
        ec.swap(coll);

        if (event_msg_.on_endpoint_removed) {
            flag_guard_t fgd(this, flag_t::EN_FT_IN_CALLBACK);
            for (endpoint_collection_t::iterator iter = ec.begin(); iter != ec.end(); ++iter) {
                event_msg_.on_endpoint_removed(std::cref(*this), iter->second.get(), EN_ATBUS_ERR_SUCCESS);
            }
        }

        return !ec.empty();
    }

    bool node::add_endpoint_fault(endpoint &ep) {
        size_t fault_count = ep.add_stat_fault();
        if (fault_count > conf_.fault_tolerant) {
            remove_endpoint(ep.get_id());
            return true;
        }

        return false;
    }

    void node::add_ping_timer(endpoint::ptr_t &ep) {
        if (!ep) {
            return;
        }

        if (conf_.ping_interval <= 0) {
            return;
        }
        event_timer_.ping_list.push_back(std::make_pair(event_timer_.sec + conf_.ping_interval, ep));
    }

    void node::stat_add_dispatch_times() { ++stat_.dispatch_times; }

    channel::io_stream_channel *node::get_iostream_channel() {
        if (iostream_channel_) {
            return iostream_channel_.get();
        }
        iostream_channel_.reset(new channel::io_stream_channel());

        channel::io_stream_init(iostream_channel_.get(), get_evloop(), get_iostream_conf());
        iostream_channel_->data = this;

        // callbacks
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_ACCEPTED]     = connection::iostream_on_accepted;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_CONNECTED]    = connection::iostream_on_connected;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_DISCONNECTED] = connection::iostream_on_disconnected;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_RECVED]       = connection::iostream_on_recv_cb;
        iostream_channel_->evt.callbacks[channel::io_stream_callback_evt_t::EN_FN_WRITEN]       = connection::iostream_on_written;

        return iostream_channel_.get();
    }

    node::ptr_t node::get_watcher() { return watcher_.lock(); }

    channel::io_stream_conf *node::get_iostream_conf() {
        if (iostream_conf_) {
            return iostream_conf_.get();
        }

        iostream_conf_.reset(new channel::io_stream_conf());
        channel::io_stream_init_configure(iostream_conf_.get());

        // 接收大小和msg size一致即可，可以只使用一块静态buffer
        iostream_conf_->recv_buffer_limit_size = conf_.msg_size;
        iostream_conf_->recv_buffer_max_size   = conf_.msg_size + conf_.msg_size;

        iostream_conf_->send_buffer_static     = conf_.send_buffer_number;
        iostream_conf_->send_buffer_max_size   = conf_.send_buffer_size;
        iostream_conf_->send_buffer_limit_size = conf_.msg_size;
        iostream_conf_->confirm_timeout        = conf_.first_idle_timeout;
        iostream_conf_->backlog                = conf_.backlog;

        return iostream_conf_.get();
    }



    node::stat_info_t::stat_info_t() : dispatch_times(0) {}
} // namespace atbus
