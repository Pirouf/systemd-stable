/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/if.h>
#include <linux/can/netlink.h>

#include "netlink-util.h"
#include "networkd-can.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "parse-util.h"
#include "string-util.h"

#define CAN_TERMINATION_OHM_VALUE 120

int config_parse_can_bitrate(
                const char* unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint32_t *br = data;
        uint64_t sz;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = parse_size(rvalue, 1000, &sz);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse can bitrate '%s', ignoring: %m", rvalue);
                return 0;
        }

        /* Linux uses __u32 for bitrates, so the value should not exceed that. */
        if (sz <= 0 || sz > UINT32_MAX) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Bit rate out of permitted range 1...4294967295");
                return 0;
        }

        *br = (uint32_t) sz;

        return 0;
}

static int link_set_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(link);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_message_warning_errno(link, m, r, "Failed to configure CAN link");
                link_enter_failed(link);
                return 1;
        }

        log_link_debug(link, "Link set");

        return 1;
}

static int link_set_can(Link *link) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        struct can_ctrlmode cm = {};
        int r;

        assert(link);
        assert(link->network);
        assert(link->manager);
        assert(link->manager->rtnl);

        log_link_debug(link, "Configuring CAN link.");

        r = sd_rtnl_message_new_link(link->manager->rtnl, &m, RTM_NEWLINK, link->ifindex);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to allocate netlink message: %m");

        r = sd_netlink_message_set_flags(m, NLM_F_REQUEST | NLM_F_ACK);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not set netlink flags: %m");

        r = sd_netlink_message_open_container(m, IFLA_LINKINFO);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to open netlink container: %m");

        r = sd_netlink_message_open_container_union(m, IFLA_INFO_DATA, link->kind);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not append IFLA_INFO_DATA attribute: %m");

        if (link->network->can_bitrate > 0 || link->network->can_sample_point > 0) {
                struct can_bittiming bt = {
                        .bitrate = link->network->can_bitrate,
                        .sample_point = link->network->can_sample_point,
                };

                log_link_debug(link, "Setting bitrate = %d bit/s", bt.bitrate);
                if (link->network->can_sample_point > 0)
                        log_link_debug(link, "Setting sample point = %d.%d%%", bt.sample_point / 10, bt.sample_point % 10);
                else
                        log_link_debug(link, "Using default sample point");

                r = sd_netlink_message_append_data(m, IFLA_CAN_BITTIMING, &bt, sizeof(bt));
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFLA_CAN_BITTIMING attribute: %m");
        }

        if (link->network->can_data_bitrate > 0 || link->network->can_data_sample_point > 0) {
                struct can_bittiming bt = {
                        .bitrate = link->network->can_data_bitrate,
                        .sample_point = link->network->can_data_sample_point,
                };

                log_link_debug(link, "Setting data bitrate = %d bit/s", bt.bitrate);
                if (link->network->can_data_sample_point > 0)
                        log_link_debug(link, "Setting data sample point = %d.%d%%", bt.sample_point / 10, bt.sample_point % 10);
                else
                        log_link_debug(link, "Using default data sample point");

                r = sd_netlink_message_append_data(m, IFLA_CAN_DATA_BITTIMING, &bt, sizeof(bt));
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFLA_CAN_DATA_BITTIMING attribute: %m");
        }

        if (link->network->can_fd_mode >= 0) {
                cm.mask |= CAN_CTRLMODE_FD;
                SET_FLAG(cm.flags, CAN_CTRLMODE_FD, link->network->can_fd_mode);
                log_link_debug(link, "Setting FD mode to '%s'.", yes_no(link->network->can_fd_mode));
        }

        if (link->network->can_non_iso >= 0) {
                cm.mask |= CAN_CTRLMODE_FD_NON_ISO;
                SET_FLAG(cm.flags, CAN_CTRLMODE_FD_NON_ISO, link->network->can_non_iso);
                log_link_debug(link, "Setting FD non-ISO mode to '%s'.", yes_no(link->network->can_non_iso));
        }

        if (link->network->can_restart_us > 0) {
                char time_string[FORMAT_TIMESPAN_MAX];
                uint64_t restart_ms;

                if (link->network->can_restart_us == USEC_INFINITY)
                        restart_ms = 0;
                else
                        restart_ms = DIV_ROUND_UP(link->network->can_restart_us, USEC_PER_MSEC);

                format_timespan(time_string, FORMAT_TIMESPAN_MAX, restart_ms * 1000, MSEC_PER_SEC);

                if (restart_ms > UINT32_MAX)
                        return log_link_error_errno(link, SYNTHETIC_ERRNO(ERANGE), "restart timeout (%s) too big.", time_string);

                log_link_debug(link, "Setting restart = %s", time_string);

                r = sd_netlink_message_append_u32(m, IFLA_CAN_RESTART_MS, restart_ms);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFLA_CAN_RESTART_MS attribute: %m");
        }

        if (link->network->can_triple_sampling >= 0) {
                cm.mask |= CAN_CTRLMODE_3_SAMPLES;
                SET_FLAG(cm.flags, CAN_CTRLMODE_3_SAMPLES, link->network->can_triple_sampling);
                log_link_debug(link, "Setting triple-sampling to '%s'.", yes_no(link->network->can_triple_sampling));
        }

        if (link->network->can_berr_reporting >= 0) {
                cm.mask |= CAN_CTRLMODE_BERR_REPORTING;
                SET_FLAG(cm.flags, CAN_CTRLMODE_BERR_REPORTING, link->network->can_berr_reporting);
                log_link_debug(link, "Setting bus error reporting to '%s'.", yes_no(link->network->can_berr_reporting));
        }

        if (link->network->can_listen_only >= 0) {
                cm.mask |= CAN_CTRLMODE_LISTENONLY;
                SET_FLAG(cm.flags, CAN_CTRLMODE_LISTENONLY, link->network->can_listen_only);
                log_link_debug(link, "Setting listen-only mode to '%s'.", yes_no(link->network->can_listen_only));
        }

        if (cm.mask != 0) {
                r = sd_netlink_message_append_data(m, IFLA_CAN_CTRLMODE, &cm, sizeof(cm));
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFLA_CAN_CTRLMODE attribute: %m");
        }

        if (link->network->can_termination >= 0) {

                log_link_debug(link, "Setting can-termination to '%s'.", yes_no(link->network->can_termination));

                r = sd_netlink_message_append_u16(m, IFLA_CAN_TERMINATION,
                                link->network->can_termination ? CAN_TERMINATION_OHM_VALUE : 0);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append IFLA_CAN_TERMINATION attribute: %m");

        }

        r = sd_netlink_message_close_container(m);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to close netlink container: %m");

        r = sd_netlink_message_close_container(m);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to close netlink container: %m");

        r = netlink_call_async(link->manager->rtnl, NULL, m, link_set_handler,
                               link_netlink_destroy_callback, link);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not send rtnetlink message: %m");

        link_ref(link);

        if (!(link->flags & IFF_UP))
                return link_up(link);

        return 0;
}

static int link_down_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        assert(link);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 1;

        r = sd_netlink_message_get_errno(m);
        if (r < 0) {
                log_link_message_warning_errno(link, m, r, "Could not bring down interface");
                link_enter_failed(link);
                return 1;
        }

        r = link_set_can(link);
        if (r < 0)
                link_enter_failed(link);

        return 1;
}

int link_configure_can(Link *link) {
        int r;

        link_set_state(link, LINK_STATE_CONFIGURING);

        if (streq_ptr(link->kind, "can")) {
                /* The CAN interface must be down to configure bitrate, etc... */
                if ((link->flags & IFF_UP)) {
                        r = link_down(link, link_down_handler);
                        if (r < 0) {
                                link_enter_failed(link);
                                return r;
                        }
                } else {
                        r = link_set_can(link);
                        if (r < 0) {
                                link_enter_failed(link);
                                return r;
                        }
                }

                return 0;
        }

        if (!(link->flags & IFF_UP)) {
                r = link_up(link);
                if (r < 0) {
                        link_enter_failed(link);
                        return r;
                }
        }

        return 0;
}
