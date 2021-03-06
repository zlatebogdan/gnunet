/*
 This file is part of GNUnet.
 (C) 2011 Christian Grothoff (and other contributing authors)

 GNUnet is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published
 by the Free Software Foundation; either version 3, or (at your
 option) any later version.

 GNUnet is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with GNUnet; see the file COPYING.  If not, write to the
 Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 Boston, MA 02111-1307, USA.
 */

/**
 * @file src/transport/gnunet-transport.c
 * @brief Tool to help configure, measure and control the transport subsystem.
 * @author Christian Grothoff
 * @author Nathan Evans
 *
 * This utility can be used to test if a transport mechanism for
 * GNUnet is properly configured.
 */

#include "platform.h"
#include "gnunet_util_lib.h"
#include "gnunet_resolver_service.h"
#include "gnunet_protocols.h"
#include "gnunet_transport_service.h"
#include "gnunet_nat_lib.h"

/**
 * How long do we wait for the NAT test to report success?
 * Should match NAT_SERVER_TIMEOUT in 'nat_test.c'.
 */
#define TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 60)
#define RESOLUTION_TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30)
#define OP_TIMEOUT GNUNET_TIME_relative_multiply (GNUNET_TIME_UNIT_SECONDS, 30)

/**
 * Benchmarking block size in KB
 */
#define BLOCKSIZE 4

/**
 * Which peer should we connect to?
 */
static char *cpid;

/**
 * Handle to transport service.
 */
static struct GNUNET_TRANSPORT_Handle *handle;

/**
 * Configuration handle
 */
static struct GNUNET_CONFIGURATION_Handle *cfg;

/**
 * Try_connect handle
 */
struct GNUNET_TRANSPORT_TryConnectHandle * tc_handle;

/**
 * Option -s.
 */
static int benchmark_send;

/**
 * Option -b.
 */
static int benchmark_receive;

/**
 * Option -l.
 */
static int benchmark_receive;

/**
 * Option -i.
 */
static int iterate_connections;

/**
 * Option -d.
 */
static int iterate_validation;

/**
 * Option -a.
 */
static int iterate_all;

/**
 * Option -t.
 */
static int test_configuration;

/**
 * Option -c.
 */
static int monitor_connects;

/**
 * Option -m.
 */
static int monitor_connections;

/**
 * Option -f.
 */
static int monitor_validation;

/**
 * Option -C.
 */
static int try_connect;

/**
 * Option -n.
 */
static int numeric;

/**
 * Global return value (0 success).
 */
static int ret;

/**
 * Current number of connections in monitor mode
 */
static int monitor_connect_counter;

/**
 * Number of bytes of traffic we received so far.
 */
static unsigned long long traffic_received;

/**
 * Number of bytes of traffic we sent so far.
 */
static unsigned long long traffic_sent;

/**
 * Starting time of transmitting/receiving data.
 */
static struct GNUNET_TIME_Absolute start_time;

/**
 * Handle for current transmission request.
 */
static struct GNUNET_TRANSPORT_TransmitHandle *th;

/**
 * Map storing information about monitored peers
 */
static struct GNUNET_CONTAINER_MultiPeerMap *monitored_peers;

/**
 *
 */
static struct GNUNET_TRANSPORT_PeerMonitoringContext *pic;

static struct GNUNET_TRANSPORT_ValidationMonitoringContext *vic;

/**
 * Identity of the peer we transmit to / connect to.
 * (equivalent to 'cpid' string).
 */
static struct GNUNET_PeerIdentity pid;

/**
 * Task scheduled for cleanup / termination of the process.
 */
static GNUNET_SCHEDULER_TaskIdentifier end;

/**
 * Task for operation timeout
 */
static GNUNET_SCHEDULER_TaskIdentifier op_timeout;

/**
 * Selected level of verbosity.
 */
static int verbosity;

/**
 * Resolver process handle.
 */
struct GNUNET_OS_Process *resolver;

/**
 * Number of tasks running that still need the resolver.
 */
static unsigned int resolver_users;

/**
 * Number of address resolutions pending
 */
static unsigned int address_resolutions;

/**
 * Address resolutions pending in progress
 */
static unsigned int address_resolution_in_progress;


/**
 * Context for a plugin test.
 */
struct TestContext
{

  /**
   * Handle to the active NAT test.
   */
  struct GNUNET_NAT_Test *tst;

  /**
   * Task identifier for the timeout.
   */
  GNUNET_SCHEDULER_TaskIdentifier tsk;

  /**
   * Name of plugin under test.
   */
  const char *name;

};

struct MonitoredPeer
{
  enum GNUNET_TRANSPORT_PeerState state;
  struct GNUNET_TIME_Absolute state_timeout;
  struct GNUNET_HELLO_Address *address;
};


 int destroy_it (void *cls,
    const struct GNUNET_PeerIdentity *key,
    void *value)
{
   struct MonitoredPeer *m = value;
   GNUNET_CONTAINER_multipeermap_remove (monitored_peers, key, value);
   GNUNET_free_non_null (m->address);
   GNUNET_free (value);
   return GNUNET_OK;
}

/**
 * Task run in monitor mode when the user presses CTRL-C to abort.
 * Stops monitoring activity.
 *
 * @param cls the 'struct GNUNET_TRANSPORT_PeerIterateContext *'
 * @param tc scheduler context
 */
static void
shutdown_task (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct GNUNET_TIME_Relative duration;
  end = GNUNET_SCHEDULER_NO_TASK;
  if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
  {
    GNUNET_SCHEDULER_cancel (op_timeout);
    op_timeout = GNUNET_SCHEDULER_NO_TASK;
  }
  if (NULL != tc_handle)
  {
    GNUNET_TRANSPORT_try_connect_cancel (tc_handle);
    tc_handle = NULL;
  }
  if (NULL != pic)
  {
    GNUNET_TRANSPORT_monitor_peers_cancel (pic);
    pic = NULL;
  }
  if (NULL != vic)
  {
    GNUNET_TRANSPORT_monitor_validation_entries_cancel (vic);
    vic = NULL;
  }
  if (NULL != th)
  {
    GNUNET_TRANSPORT_notify_transmit_ready_cancel (th);
    th = NULL;
  }
  if (NULL != handle)
  {
    GNUNET_TRANSPORT_disconnect (handle);
    handle = NULL;
  }
  if (benchmark_send)
  {
    duration = GNUNET_TIME_absolute_get_duration (start_time);
    FPRINTF (stdout, _("Transmitted %llu bytes/s (%llu bytes in %s)\n"),
        1000LL * 1000LL * traffic_sent / (1 + duration.rel_value_us),
        traffic_sent,
        GNUNET_STRINGS_relative_time_to_string (duration, GNUNET_YES));
  }
  if (benchmark_receive)
  {
    duration = GNUNET_TIME_absolute_get_duration (start_time);
    FPRINTF (stdout, _("Received %llu bytes/s (%llu bytes in %s)\n"),
        1000LL * 1000LL * traffic_received / (1 + duration.rel_value_us),
        traffic_received,
        GNUNET_STRINGS_relative_time_to_string (duration, GNUNET_YES));
  }

  if (NULL != monitored_peers)
  {
    GNUNET_CONTAINER_multipeermap_iterate (monitored_peers, &destroy_it, NULL);
    GNUNET_CONTAINER_multipeermap_destroy (monitored_peers);
    monitored_peers = NULL;
  }
}

static struct PeerResolutionContext *rc_head;
static struct PeerResolutionContext *rc_tail;

struct PeerResolutionContext
{
  struct PeerResolutionContext *next;
  struct PeerResolutionContext *prev;
  struct GNUNET_PeerIdentity id;
  struct GNUNET_HELLO_Address *addrcp;
  struct GNUNET_TRANSPORT_AddressToStringContext *asc;
  enum GNUNET_TRANSPORT_PeerState state;
  struct GNUNET_TIME_Absolute state_timeout;
  char *transport;
  int printed;
};

static struct ValidationResolutionContext *vc_head;
static struct ValidationResolutionContext *vc_tail;

struct ValidationResolutionContext
{
  struct ValidationResolutionContext *next;
  struct ValidationResolutionContext *prev;

  struct GNUNET_PeerIdentity id;
  struct GNUNET_HELLO_Address *addrcp;
  struct GNUNET_TIME_Absolute last_validation;
  struct GNUNET_TIME_Absolute valid_until;
  struct GNUNET_TIME_Absolute next_validation;
  enum GNUNET_TRANSPORT_ValidationState state;

  struct GNUNET_TRANSPORT_AddressToStringContext *asc;

  char *transport;
  int printed;
};

static void
operation_timeout (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct PeerResolutionContext *cur;
  struct PeerResolutionContext *next;
  op_timeout = GNUNET_SCHEDULER_NO_TASK;
  if ((try_connect) || (benchmark_send) || (benchmark_receive))
  {
    FPRINTF (stdout, _("Failed to connect to `%s'\n"), GNUNET_i2s_full (&pid));
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    ret = 1;
    return;
  }
  if (iterate_connections)
  {
    next = rc_head;
    while (NULL != (cur = next))
    {
      next = cur->next;
      FPRINTF (stdout, _("Failed to resolve address for peer `%s'\n"),
          GNUNET_i2s (&cur->addrcp->peer));

      GNUNET_CONTAINER_DLL_remove(rc_head, rc_tail, cur);
      GNUNET_TRANSPORT_address_to_string_cancel (cur->asc);
      GNUNET_free(cur->transport);
      GNUNET_free(cur->addrcp);
      GNUNET_free(cur);

    }
    FPRINTF (stdout, "%s", _("Failed to list connections, timeout occured\n") );
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    ret = 1;
    return;
  }

}

/**
 * Display the result of the test.
 *
 * @param tc test context
 * @param result #GNUNET_YES on success
 */
static void
display_test_result (struct TestContext *tc, int result)
{
  if (GNUNET_YES != result)
  {
    FPRINTF (stderr, "Configuration for plugin `%s' did not work!\n", tc->name);
  }
  else
  {
    FPRINTF (stderr, "Configuration for plugin `%s' is working!\n", tc->name);
  }
  if (GNUNET_SCHEDULER_NO_TASK != tc->tsk)
  {
    GNUNET_SCHEDULER_cancel (tc->tsk);
    tc->tsk = GNUNET_SCHEDULER_NO_TASK;
  }
  if (NULL != tc->tst)
  {
    GNUNET_NAT_test_stop (tc->tst);
    tc->tst = NULL;
  }
  GNUNET_free(tc);
  resolver_users--;
  if ((0 == resolver_users) && (NULL != resolver))
  {
    GNUNET_break(0 == GNUNET_OS_process_kill (resolver, GNUNET_TERM_SIG));
    GNUNET_OS_process_destroy (resolver);
    resolver = NULL;
  }
}

/**
 * Function called by NAT on success.
 * Clean up and update GUI (with success).
 *
 * @param cls test context
 * @param success currently always #GNUNET_OK
 * @param emsg error message, NULL on success
 */
static void
result_callback (void *cls, int success, const char *emsg)
{
  struct TestContext *tc = cls;

  display_test_result (tc, success);
}

/**
 * Function called if NAT failed to confirm success.
 * Clean up and update GUI (with failure).
 *
 * @param cls test context
 * @param tc scheduler callback
 */
static void
fail_timeout (void *cls, const struct GNUNET_SCHEDULER_TaskContext *tc)
{
  struct TestContext *tstc = cls;

  tstc->tsk = GNUNET_SCHEDULER_NO_TASK;
  display_test_result (tstc, GNUNET_NO);
}


static void
resolve_validation_address (const struct GNUNET_PeerIdentity *id,
    const struct GNUNET_HELLO_Address *address, int numeric,
    struct GNUNET_TIME_Absolute last_validation,
    struct GNUNET_TIME_Absolute valid_until,
    struct GNUNET_TIME_Absolute next_validation,
    enum GNUNET_TRANSPORT_ValidationState state);

static void
process_validation_string (void *cls, const char *address)
{
  struct ValidationResolutionContext *vc = cls;
  char *s_valid;
  char *s_last;
  char *s_next;

  if (address != NULL )
  {
    if (GNUNET_TIME_UNIT_ZERO_ABS.abs_value_us == vc->valid_until.abs_value_us)
      s_valid = GNUNET_strdup("never");
    else
      s_valid = GNUNET_strdup(GNUNET_STRINGS_absolute_time_to_string (vc->valid_until));

    if (GNUNET_TIME_UNIT_ZERO_ABS.abs_value_us == vc->last_validation.abs_value_us)
      s_last = GNUNET_strdup("never");
    else
      s_last = GNUNET_strdup(GNUNET_STRINGS_absolute_time_to_string (vc->last_validation));

    if (GNUNET_TIME_UNIT_ZERO_ABS.abs_value_us == vc->next_validation.abs_value_us)
      s_next = GNUNET_strdup("never");
    else
      s_next = GNUNET_strdup(GNUNET_STRINGS_absolute_time_to_string (vc->next_validation));

    FPRINTF (stdout,
        _("Peer `%s' %s %s\n\t%s%s\n\t%s%s\n\t%s%s\n"),
        GNUNET_i2s (&vc->id), address,
        (monitor_validation) ? GNUNET_TRANSPORT_vs2s (vc->state) : "",
        "Valid until    : ", s_valid,
        "Last validation: ",s_last,
        "Next validation: ", s_next);
    GNUNET_free (s_valid);
    GNUNET_free (s_last);
    GNUNET_free (s_next);
    vc->printed = GNUNET_YES;
  }
  else
  {
    /* done */
    GNUNET_assert(address_resolutions > 0);
    address_resolutions--;
    if (GNUNET_NO == vc->printed)
    {
      if (numeric == GNUNET_NO)
      {
        /* Failed to resolve address, try numeric lookup */
        resolve_validation_address (&vc->id, vc->addrcp, GNUNET_NO,
           vc->last_validation, vc->valid_until, vc->next_validation,
           vc->state);
      }
      else
      {
        FPRINTF (stdout, _("Peer `%s' %s `%s' \n"),
            GNUNET_i2s (&vc->id), "<unable to resolve address>",
            GNUNET_TRANSPORT_vs2s (vc->state));
      }
    }
    GNUNET_free (vc->transport);
    GNUNET_free (vc->addrcp);
    GNUNET_CONTAINER_DLL_remove(vc_head, vc_tail, vc);
    GNUNET_free(vc);
    if ((0 == address_resolutions) && (iterate_validation))
    {
      if (GNUNET_SCHEDULER_NO_TASK != end)
      {
        GNUNET_SCHEDULER_cancel (end);
        end = GNUNET_SCHEDULER_NO_TASK;
      }
      if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
      {
        GNUNET_SCHEDULER_cancel (op_timeout);
        op_timeout = GNUNET_SCHEDULER_NO_TASK;
      }
      ret = 0;
      end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    }
  }
}



static void
resolve_validation_address (const struct GNUNET_PeerIdentity *id,
    const struct GNUNET_HELLO_Address *address, int numeric,
    struct GNUNET_TIME_Absolute last_validation,
    struct GNUNET_TIME_Absolute valid_until,
    struct GNUNET_TIME_Absolute next_validation,
    enum GNUNET_TRANSPORT_ValidationState state)
{
  struct ValidationResolutionContext *vc;

  vc = GNUNET_new (struct ValidationResolutionContext);
  GNUNET_assert(NULL != vc);
  GNUNET_CONTAINER_DLL_insert(vc_head, vc_tail, vc);
  address_resolutions++;

  vc->id = (*id);
  vc->transport = GNUNET_strdup(address->transport_name);
  vc->addrcp = GNUNET_HELLO_address_copy (address);
  vc->printed = GNUNET_NO;
  vc->state = state;
  vc->last_validation = last_validation;
  vc->valid_until = valid_until;
  vc->next_validation = next_validation;

  /* Resolve address to string */
  vc->asc = GNUNET_TRANSPORT_address_to_string (cfg, address, numeric,
      RESOLUTION_TIMEOUT, &process_validation_string, vc);
}


void process_validation_cb (void *cls,
    const struct GNUNET_PeerIdentity *peer,
    const struct GNUNET_HELLO_Address *address,
    struct GNUNET_TIME_Absolute last_validation,
    struct GNUNET_TIME_Absolute valid_until,
    struct GNUNET_TIME_Absolute next_validation,
    enum GNUNET_TRANSPORT_ValidationState state)
{
  if ((NULL == peer) && (NULL == address))
  {
    /* done */
    vic = NULL;
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    return;
  }
  if ((NULL == peer) || (NULL == address))
  {
    /* invalid response */
    vic = NULL;
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    return;
  }
  resolve_validation_address (peer, address,
     numeric, last_validation,
     valid_until, next_validation, state);
}

/**
 * Test our plugin's configuration (NAT traversal, etc.).
 *
 * @param cfg configuration to test
 */
static void
do_test_configuration (const struct GNUNET_CONFIGURATION_Handle *cfg)
{
  char *plugins;
  char *tok;
  unsigned long long bnd_port;
  unsigned long long adv_port;
  struct TestContext *tc;
  char *binary;

  if (GNUNET_OK
      != GNUNET_CONFIGURATION_get_value_string (cfg, "transport", "plugins",
          &plugins))
  {
    FPRINTF (stderr, "%s", _
    ("No transport plugins configured, peer will never communicate\n") );
    ret = 4;
    return;
  }
  for (tok = strtok (plugins, " "); tok != NULL ; tok = strtok (NULL, " "))
  {
    char section[12 + strlen (tok)];

    GNUNET_snprintf (section, sizeof(section), "transport-%s", tok);
    if (GNUNET_OK
        != GNUNET_CONFIGURATION_get_value_number (cfg, section, "PORT",
            &bnd_port))
    {
      FPRINTF (stderr,
          _("No port configured for plugin `%s', cannot test it\n"), tok);
      continue;
    }
    if (GNUNET_OK
        != GNUNET_CONFIGURATION_get_value_number (cfg, section,
            "ADVERTISED_PORT", &adv_port))
      adv_port = bnd_port;
    if (NULL == resolver)
    {
      binary = GNUNET_OS_get_libexec_binary_path ("gnunet-service-resolver");
      resolver = GNUNET_OS_start_process (GNUNET_YES,
                                          GNUNET_OS_INHERIT_STD_OUT_AND_ERR,
                                          NULL, NULL, NULL,
                                          binary,
                                          "gnunet-service-resolver", NULL );
      GNUNET_free(binary);
    }
    resolver_users++;
    GNUNET_RESOLVER_connect (cfg);
    tc = GNUNET_new (struct TestContext);
    tc->name = GNUNET_strdup (tok);
    tc->tst = GNUNET_NAT_test_start (cfg,
        (0 == strcasecmp (tok, "udp")) ? GNUNET_NO : GNUNET_YES,
        (uint16_t) bnd_port, (uint16_t) adv_port, &result_callback, tc);
    if (NULL == tc->tst)
    {
      display_test_result (tc, GNUNET_SYSERR);
      continue;
    }
    tc->tsk = GNUNET_SCHEDULER_add_delayed (TIMEOUT, &fail_timeout, tc);
  }
  GNUNET_free(plugins);
}

/**
 * Function called to notify a client about the socket
 * begin ready to queue more data.  @a buf will be
 * NULL and @a size zero if the socket was closed for
 * writing in the meantime.
 *
 * @param cls closure
 * @param size number of bytes available in @a buf
 * @param buf where the callee should write the message
 * @return number of bytes written to @a buf
 */
static size_t
transmit_data (void *cls, size_t size, void *buf)
{
  struct GNUNET_MessageHeader *m = buf;

  if ((NULL == buf) || (0 == size))
  {
    th = NULL;
    return 0;
  }

  GNUNET_assert(size >= sizeof(struct GNUNET_MessageHeader));
  GNUNET_assert(size < GNUNET_SERVER_MAX_MESSAGE_SIZE);
  m->size = ntohs (size);
  m->type = ntohs (GNUNET_MESSAGE_TYPE_DUMMY);
  memset (&m[1], 52, size - sizeof(struct GNUNET_MessageHeader));
  traffic_sent += size;
  th = GNUNET_TRANSPORT_notify_transmit_ready (handle, &pid,
                                               BLOCKSIZE * 1024,
                                               GNUNET_TIME_UNIT_FOREVER_REL,
                                               &transmit_data, NULL );
  if (verbosity > 0)
    FPRINTF (stdout, _("Transmitting %u bytes to %s\n"), (unsigned int) size,
        GNUNET_i2s (&pid));
  return size;
}

/**
 * Function called to notify transport users that another
 * peer connected to us.
 *
 * @param cls closure
 * @param peer the peer that connected
 */
static void
notify_connect (void *cls, const struct GNUNET_PeerIdentity *peer)
{
  if (0 != memcmp (&pid, peer, sizeof(struct GNUNET_PeerIdentity)))
    return;
  ret = 0;
  if (try_connect)
  {
    /* all done, terminate instantly */
    FPRINTF (stdout, _("Successfully connected to `%s'\n"),
        GNUNET_i2s_full (peer));
    ret = 0;

    if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
    {
      GNUNET_SCHEDULER_cancel (op_timeout);
      op_timeout = GNUNET_SCHEDULER_NO_TASK;
    }

    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    return;
  }
  if (benchmark_send)
  {
    if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
    {
      GNUNET_SCHEDULER_cancel (op_timeout);
      op_timeout = GNUNET_SCHEDULER_NO_TASK;
    }
    if (verbosity > 0)
      FPRINTF (stdout,
          _("Successfully connected to `%s', starting to send benchmark data in %u Kb blocks\n"),
          GNUNET_i2s (&pid), BLOCKSIZE);
    start_time = GNUNET_TIME_absolute_get ();
    if (NULL == th)
      th = GNUNET_TRANSPORT_notify_transmit_ready (handle, peer,
                                                   BLOCKSIZE * 1024,
                                                   GNUNET_TIME_UNIT_FOREVER_REL,
                                                   &transmit_data,
                                                   NULL);
    else
      GNUNET_break(0);
    return;
  }
}

/**
 * Function called to notify transport users that another
 * peer disconnected from us.
 *
 * @param cls closure
 * @param peer the peer that disconnected
 */
static void
notify_disconnect (void *cls, const struct GNUNET_PeerIdentity *peer)
{
  if (0 != memcmp (&pid, peer, sizeof(struct GNUNET_PeerIdentity)))
    return;

  if (NULL != th)
  {
    GNUNET_TRANSPORT_notify_transmit_ready_cancel (th);
    th = NULL;
  }
  if (benchmark_send)
  {
    FPRINTF (stdout, _("Disconnected from peer `%s' while benchmarking\n"),
        GNUNET_i2s (&pid));
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    return;
  }
}

/**
 * Function called to notify transport users that another
 * peer connected to us.
 *
 * @param cls closure
 * @param peer the peer that connected
 */
static void
monitor_notify_connect (void *cls, const struct GNUNET_PeerIdentity *peer)
{
  monitor_connect_counter++;
  struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
  const char *now_str = GNUNET_STRINGS_absolute_time_to_string (now);

  FPRINTF (stdout, _("%24s: %-17s %4s   (%u connections in total)\n"), now_str,
      _("Connected to"), GNUNET_i2s (peer), monitor_connect_counter);
}

/**
 * Function called to notify transport users that another
 * peer disconnected from us.
 *
 * @param cls closure
 * @param peer the peer that disconnected
 */
static void
monitor_notify_disconnect (void *cls, const struct GNUNET_PeerIdentity *peer)
{
  struct GNUNET_TIME_Absolute now = GNUNET_TIME_absolute_get ();
  const char *now_str = GNUNET_STRINGS_absolute_time_to_string (now);

  GNUNET_assert(monitor_connect_counter > 0);
  monitor_connect_counter--;

  FPRINTF (stdout, _("%24s: %-17s %4s   (%u connections in total)\n"), now_str,
      _("Disconnected from"), GNUNET_i2s (peer), monitor_connect_counter);
}

/**
 * Function called by the transport for each received message.
 *
 * @param cls closure
 * @param peer (claimed) identity of the other peer
 * @param message the message
 */
static void
notify_receive (void *cls, const struct GNUNET_PeerIdentity *peer,
    const struct GNUNET_MessageHeader *message)
{
  if (benchmark_receive)
  {
    if (GNUNET_MESSAGE_TYPE_DUMMY != ntohs (message->type))
      return;
    if (verbosity > 0)
      FPRINTF (stdout, _("Received %u bytes from %s\n"),
          (unsigned int) ntohs (message->size), GNUNET_i2s (peer));

    if (traffic_received == 0)
      start_time = GNUNET_TIME_absolute_get ();
    traffic_received += ntohs (message->size);
    return;
  }
}

static void
resolve_peer_address (const struct GNUNET_PeerIdentity *id,
    const struct GNUNET_HELLO_Address *address, int numeric,
    enum GNUNET_TRANSPORT_PeerState state,
    struct GNUNET_TIME_Absolute state_timeout);

static void
print_info (const struct GNUNET_PeerIdentity *id, const char *transport,
    const char *addr, enum GNUNET_TRANSPORT_PeerState state,
    struct GNUNET_TIME_Absolute state_timeout)
{
  if ((GNUNET_YES == iterate_all) || (GNUNET_YES == monitor_connections) )
  {
    FPRINTF (stdout, _("Peer `%s': %s %s in state `%s' until %s\n"),
        GNUNET_i2s (id),
        (NULL == transport) ? "<none>" : transport,
        (NULL == transport) ? "<none>" : addr,
        GNUNET_TRANSPORT_ps2s (state),
        GNUNET_STRINGS_absolute_time_to_string (state_timeout));
  }
  else
  {
    /* Only connected peers, skip state */
    FPRINTF (stdout, _("Peer `%s': %s %s\n"), GNUNET_i2s (id), transport, addr);
  }

}

static void
process_peer_string (void *cls, const char *address)
{
  struct PeerResolutionContext *rc = cls;

  if (address != NULL )
  {
    print_info (&rc->id, rc->transport, address, rc->state, rc->state_timeout);
    rc->printed = GNUNET_YES;
  }
  else
  {
    /* done */
    GNUNET_assert(address_resolutions > 0);
    address_resolutions--;
    if (GNUNET_NO == rc->printed)
    {
      if (numeric == GNUNET_NO)
      {
        /* Failed to resolve address, try numeric lookup */
        resolve_peer_address (&rc->id, rc->addrcp, GNUNET_YES,
            rc->state, rc->state_timeout);
      }
      else
      {
        print_info (&rc->id, rc->transport, "<unable to resolve address>",
            rc->state, rc->state_timeout);
      }
    }
    GNUNET_free (rc->transport);
    GNUNET_free (rc->addrcp);
    GNUNET_CONTAINER_DLL_remove(rc_head, rc_tail, rc);
    GNUNET_free(rc);
    if ((0 == address_resolutions) && (iterate_connections))
    {
      if (GNUNET_SCHEDULER_NO_TASK != end)
      {
        GNUNET_SCHEDULER_cancel (end);
        end = GNUNET_SCHEDULER_NO_TASK;
      }
      if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
      {
        GNUNET_SCHEDULER_cancel (op_timeout);
        op_timeout = GNUNET_SCHEDULER_NO_TASK;
      }
      ret = 0;
      end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    }
  }
}

static void
resolve_peer_address (const struct GNUNET_PeerIdentity *id,
    const struct GNUNET_HELLO_Address *address, int numeric,
    enum GNUNET_TRANSPORT_PeerState state,
    struct GNUNET_TIME_Absolute state_timeout)
{
  struct PeerResolutionContext *rc;

  rc = GNUNET_new (struct PeerResolutionContext);
  GNUNET_assert(NULL != rc);
  GNUNET_CONTAINER_DLL_insert(rc_head, rc_tail, rc);
  address_resolutions++;

  rc->id = (*id);
  rc->transport = GNUNET_strdup(address->transport_name);
  rc->addrcp = GNUNET_HELLO_address_copy (address);
  rc->printed = GNUNET_NO;
  rc->state = state;
  rc->state_timeout = state_timeout;
  /* Resolve address to string */
  rc->asc = GNUNET_TRANSPORT_address_to_string (cfg, address, numeric,
      RESOLUTION_TIMEOUT, &process_peer_string, rc);
}

/**
 * Function called with information about a peers during a one shot iteration
 *
 * @param cls closure
 * @param peer identity of the peer, NULL for final callback when operation done
 * @param address binary address used to communicate with this peer,
 *  NULL on disconnect or when done
 * @param state current state this peer is in
 * @param state_timeout time out for the current state
 *
 */
static void
process_peer_iteration_cb (void *cls, const struct GNUNET_PeerIdentity *peer,
    const struct GNUNET_HELLO_Address *address,
    enum GNUNET_TRANSPORT_PeerState state,
    struct GNUNET_TIME_Absolute state_timeout)
{
  if (peer == NULL )
  {
    /* done */
    address_resolution_in_progress = GNUNET_NO;
    pic = NULL;
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    return;
  }

  if ((GNUNET_NO == iterate_all) && (GNUNET_NO == GNUNET_TRANSPORT_is_connected(state)) )
      return; /* Display only connected peers */

  if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
    GNUNET_SCHEDULER_cancel (op_timeout);
  op_timeout = GNUNET_SCHEDULER_add_delayed (OP_TIMEOUT, &operation_timeout,
      NULL );

  GNUNET_log(GNUNET_ERROR_TYPE_DEBUG, "Received address for peer `%s': %s\n",
      GNUNET_i2s (peer), address->transport_name);

  if (NULL != address)
    resolve_peer_address (peer, address, numeric, state, state_timeout);
  else
    print_info (peer, NULL, NULL, state, state_timeout);
}


/**
 * Function called with information about a peers
 *
 * @param cls closure
 * @param peer identity of the peer, NULL for final callback when operation done
 * @param address binary address used to communicate with this peer,
 *  NULL on disconnect or when done
 * @param state current state this peer is in
 * @param state_timeout time out for the current state
 *
 */
static void
process_peer_monitoring_cb (void *cls, const struct GNUNET_PeerIdentity *peer,
    const struct GNUNET_HELLO_Address *address,
    enum GNUNET_TRANSPORT_PeerState state,
    struct GNUNET_TIME_Absolute state_timeout)
{
  struct MonitoredPeer *m;

  if (peer == NULL )
  {
    /* done */
    address_resolution_in_progress = GNUNET_NO;
    pic = NULL;
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    return;
  }

  if (GNUNET_SCHEDULER_NO_TASK != op_timeout)
    GNUNET_SCHEDULER_cancel (op_timeout);
  op_timeout = GNUNET_SCHEDULER_add_delayed (OP_TIMEOUT, &operation_timeout,
      NULL );

  if (NULL == (m = GNUNET_CONTAINER_multipeermap_get (monitored_peers, peer)))
  {
    m = GNUNET_new (struct MonitoredPeer);
    GNUNET_CONTAINER_multipeermap_put (monitored_peers, peer,
        m, GNUNET_CONTAINER_MULTIHASHMAPOPTION_UNIQUE_FAST);
  }
  else
  {
    if ( (m->state == state) &&
      (m->state_timeout.abs_value_us == state_timeout.abs_value_us) &&
      ((NULL == address) && (NULL == m->address)) )
    {
      return; /* No real change */
    }
    if ( ((NULL != address) && (NULL != m->address)) &&
        (0 == GNUNET_HELLO_address_cmp(m->address, address)) )
      return; /* No real change */
  }


  if (NULL != m->address)
  {
    GNUNET_free (m->address);
    m->address = NULL;
  }
  if (NULL != address)
    m->address = GNUNET_HELLO_address_copy (address);
  m->state = state;
  m->state_timeout = state_timeout;

  if (NULL != address)
    resolve_peer_address (peer, m->address, numeric, m->state, m->state_timeout);
  else
    print_info (peer, NULL, NULL, m->state, m->state_timeout);
}

static void
try_connect_cb (void *cls, const int result)
{
  static int retries = 0;
  if (GNUNET_OK == result)
  {
    tc_handle = NULL;
    return;
  }
  retries++;
  if (retries < 10)
    tc_handle = GNUNET_TRANSPORT_try_connect (handle, &pid, try_connect_cb,
        NULL );
  else
  {
    FPRINTF (stderr, "%s",
        _("Failed to send connect request to transport service\n") );
    if (GNUNET_SCHEDULER_NO_TASK != end)
      GNUNET_SCHEDULER_cancel (end);
    ret = 1;
    end = GNUNET_SCHEDULER_add_now (&shutdown_task, NULL );
    return;
  }
}

/**
 * Function called with the result of the check if the 'transport'
 * service is running.
 *
 * @param cls closure with our configuration
 * @param result #GNUNET_YES if transport is running
 */
static void
testservice_task (void *cls, int result)
{
  int counter = 0;
  ret = 1;

  if (GNUNET_YES != result)
  {
    FPRINTF (stderr, _("Service `%s' is not running\n"), "transport");
    return;
  }

  if ((NULL != cpid)
      && (GNUNET_OK
          != GNUNET_CRYPTO_eddsa_public_key_from_string (cpid, strlen (cpid),
              &pid.public_key)))
  {
    FPRINTF (stderr, _("Failed to parse peer identity `%s'\n"), cpid);
    return;
  }

  counter = benchmark_send + benchmark_receive + iterate_connections
      + monitor_connections + monitor_connects + try_connect
      + iterate_validation + monitor_validation;

  if (1 < counter)
  {
    FPRINTF (stderr,
        _("Multiple operations given. Please choose only one operation: %s, %s, %s, %s, %s, %s\n"),
        "connect", "benchmark send", "benchmark receive", "information",
        "monitor", "events");
    return;
  }
  if (0 == counter)
  {
    FPRINTF (stderr,
        _("No operation given. Please choose one operation: %s, %s, %s, %s, %s, %s\n"),
        "connect", "benchmark send", "benchmark receive", "information",
        "monitor", "events");
    return;
  }

  if (try_connect) /* -C: Connect to peer */
  {
    if (NULL == cpid)
    {
      FPRINTF (stderr, _("Option `%s' makes no sense without option `%s'.\n"),
          "-C", "-p");
      ret = 1;
      return;
    }
    handle = GNUNET_TRANSPORT_connect (cfg, NULL, NULL, &notify_receive,
        &notify_connect, &notify_disconnect);
    if (NULL == handle)
    {
      FPRINTF (stderr, "%s", _("Failed to connect to transport service\n") );
      ret = 1;
      return;
    }
    tc_handle = GNUNET_TRANSPORT_try_connect (handle, &pid, try_connect_cb,
        NULL );
    if (NULL == tc_handle)
    {
      FPRINTF (stderr, "%s",
          _("Failed to send request to transport service\n") );
      ret = 1;
      return;
    }
    op_timeout = GNUNET_SCHEDULER_add_delayed (OP_TIMEOUT, &operation_timeout,
        NULL );

  }
  else if (benchmark_send) /* -s: Benchmark sending */
  {
    if (NULL == cpid)
    {
      FPRINTF (stderr, _("Option `%s' makes no sense without option `%s'.\n"),
          "-s", "-p");
      ret = 1;
      return;
    }
    handle = GNUNET_TRANSPORT_connect (cfg, NULL, NULL, &notify_receive,
        &notify_connect, &notify_disconnect);
    if (NULL == handle)
    {
      FPRINTF (stderr, "%s", _("Failed to connect to transport service\n") );
      ret = 1;
      return;
    }
    tc_handle = GNUNET_TRANSPORT_try_connect (handle, &pid, try_connect_cb,
        NULL );
    if (NULL == tc_handle)
    {
      FPRINTF (stderr, "%s",
          _("Failed to send request to transport service\n") );
      ret = 1;
      return;
    }
    start_time = GNUNET_TIME_absolute_get ();
    op_timeout = GNUNET_SCHEDULER_add_delayed (OP_TIMEOUT, &operation_timeout,
        NULL );
  }
  else if (benchmark_receive) /* -b: Benchmark receiving */
  {
    handle = GNUNET_TRANSPORT_connect (cfg, NULL, NULL, &notify_receive, NULL,
        NULL );
    if (NULL == handle)
    {
      FPRINTF (stderr, "%s", _("Failed to connect to transport service\n") );
      ret = 1;
      return;
    }
    if (verbosity > 0)
      FPRINTF (stdout, "%s", _("Starting to receive benchmark data\n") );
    start_time = GNUNET_TIME_absolute_get ();

  }
  else if (iterate_connections) /* -i: List information about peers once */
  {
    address_resolution_in_progress = GNUNET_YES;
    pic = GNUNET_TRANSPORT_monitor_peers (cfg, (NULL == cpid) ? NULL : &pid,
        GNUNET_YES, TIMEOUT, &process_peer_iteration_cb, (void *) cfg);
    op_timeout = GNUNET_SCHEDULER_add_delayed (OP_TIMEOUT, &operation_timeout,
        NULL );
  }
  else if (monitor_connections) /* -m: List information about peers continuously */
  {
    monitored_peers = GNUNET_CONTAINER_multipeermap_create (10, GNUNET_NO);
    address_resolution_in_progress = GNUNET_YES;
    pic = GNUNET_TRANSPORT_monitor_peers (cfg, (NULL == cpid) ? NULL : &pid,
        GNUNET_NO, TIMEOUT, &process_peer_monitoring_cb, (void *) cfg);
  }
  else if (iterate_validation) /* -d: Print information about validations */
  {
    vic = GNUNET_TRANSPORT_monitor_validation_entries (cfg,
        (NULL == cpid) ? NULL : &pid,
        GNUNET_YES, TIMEOUT, &process_validation_cb, (void *) cfg);
  }
  else if (monitor_validation) /* -f: Print information about validations continuously */
  {
    vic = GNUNET_TRANSPORT_monitor_validation_entries (cfg,
        (NULL == cpid) ? NULL : &pid,
        GNUNET_NO, TIMEOUT, &process_validation_cb, (void *) cfg);
  }
  else if (monitor_connects) /* -e : Monitor (dis)connect events continuously */
  {
    monitor_connect_counter = 0;
    handle = GNUNET_TRANSPORT_connect (cfg, NULL, NULL, NULL,
        &monitor_notify_connect, &monitor_notify_disconnect);
    if (NULL == handle)
    {
      FPRINTF (stderr, "%s", _("Failed to connect to transport service\n") );
      ret = 1;
      return;
    }
    ret = 0;
  }
  else
  {
    GNUNET_break(0);
    return;
  }

  end = GNUNET_SCHEDULER_add_delayed (GNUNET_TIME_UNIT_FOREVER_REL,
      &shutdown_task, NULL );

}

/**
 * Main function that will be run by the scheduler.
 *
 * @param cls closure
 * @param args remaining command-line arguments
 * @param cfgfile name of the configuration file used (for saving, can be NULL!)
 * @param mycfg configuration
 */
static void
run (void *cls, char * const *args, const char *cfgfile,
    const struct GNUNET_CONFIGURATION_Handle *mycfg)
{
  cfg = (struct GNUNET_CONFIGURATION_Handle *) mycfg;
  if (test_configuration)
  {
    do_test_configuration (cfg);
    return;
  }
  GNUNET_CLIENT_service_test ("transport", cfg, GNUNET_TIME_UNIT_SECONDS,
      &testservice_task, (void *) cfg);
}

int
main (int argc, char * const *argv)
{
  int res;
  static const struct GNUNET_GETOPT_CommandLineOption options[] =
      {
          { 'a', "all", NULL,
              gettext_noop ("print information for all peers (instead of only connected peers )"),
              0, &GNUNET_GETOPT_set_one, &iterate_all },
          { 'b', "benchmark", NULL,
              gettext_noop ("measure how fast we are receiving data from all peers (until CTRL-C)"),
              0, &GNUNET_GETOPT_set_one, &benchmark_receive }, { 'C', "connect",
              NULL, gettext_noop ("connect to a peer"), 0,
              &GNUNET_GETOPT_set_one, &try_connect },
          { 'd', "validation", NULL,
              gettext_noop ("print information for all pending validations "),
              0, &GNUNET_GETOPT_set_one, &iterate_validation },
          { 'f', "monitorvalidation", NULL,
              gettext_noop ("print information for all pending validations continously"),
              0, &GNUNET_GETOPT_set_one, &monitor_validation },
          { 'i', "information", NULL,
              gettext_noop ("provide information about all current connections (once)"),
              0, &GNUNET_GETOPT_set_one, &iterate_connections },
          { 'm', "monitor", NULL,
              gettext_noop ("provide information about all current connections (continuously)"),
              0, &GNUNET_GETOPT_set_one, &monitor_connections },
          { 'e', "events", NULL,
              gettext_noop ("provide information about all connects and disconnect events (continuously)"),
              0, &GNUNET_GETOPT_set_one, &monitor_connects }, { 'n', "numeric",
              NULL, gettext_noop ("do not resolve hostnames"), 0,
              &GNUNET_GETOPT_set_one, &numeric }, { 'p', "peer", "PEER",
              gettext_noop ("peer identity"), 1, &GNUNET_GETOPT_set_string,
              &cpid }, { 's', "send", NULL, gettext_noop
          ("send data for benchmarking to the other peer (until CTRL-C)"), 0,
              &GNUNET_GETOPT_set_one, &benchmark_send },
          { 't', "test", NULL,
              gettext_noop ("test transport configuration (involves external server)"),
              0, &GNUNET_GETOPT_set_one, &test_configuration },
              GNUNET_GETOPT_OPTION_VERBOSE (&verbosity),
              GNUNET_GETOPT_OPTION_END };

  if (GNUNET_OK != GNUNET_STRINGS_get_utf8_args (argc, argv, &argc, &argv))
    return 2;

  res = GNUNET_PROGRAM_run (argc, argv, "gnunet-transport", gettext_noop
  ("Direct access to transport service."), options, &run, NULL );
  GNUNET_free((void * ) argv);
  if (GNUNET_OK == res)
    return ret;
  return 1;
}

/* end of gnunet-transport.c */
