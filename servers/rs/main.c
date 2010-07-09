/* Reincarnation Server.  This servers starts new system services and detects
 * they are exiting.   In case of errors, system services can be restarted.  
 * The RS server periodically checks the status of all registered services
 * services to see whether they are still alive.   The system services are 
 * expected to periodically send a heartbeat message. 
 * 
 * Changes:
 *   Nov 22, 2009: rewrite of boot process (Cristiano Giuffrida)
 *   Jul 22, 2005: Created  (Jorrit N. Herder)
 */
#include "inc.h"
#include <fcntl.h>
#include <a.out.h>
#include <minix/crtso.h>
#include "kernel/const.h"
#include "kernel/type.h"
#include "kernel/proc.h"
#include "../pm/mproc.h"

/* Declare some local functions. */
FORWARD _PROTOTYPE(void boot_image_info_lookup, ( endpoint_t endpoint,
    struct boot_image *image,
    struct boot_image **ip, struct boot_image_priv **pp,
    struct boot_image_sys **sp, struct boot_image_dev **dp)             );
FORWARD _PROTOTYPE(void catch_boot_init_ready, (endpoint_t endpoint)	);
FORWARD _PROTOTYPE(void get_work, (message *m_ptr, int *status_ptr)	);

/* Flag set when memory unmapping can be done. */
EXTERN int unmap_ok;

/* SEF functions and variables. */
FORWARD _PROTOTYPE( void sef_local_startup, (void)                      );
FORWARD _PROTOTYPE( int sef_cb_init_fresh, (int type, sef_init_info_t *info) );
FORWARD _PROTOTYPE( void sef_cb_signal_handler, (int signo) );
FORWARD _PROTOTYPE( int sef_cb_signal_manager, (endpoint_t target, int signo) );

/*===========================================================================*
 *				main                                         *
 *===========================================================================*/
PUBLIC int main(void)
{
/* This is the main routine of this service. The main loop consists of 
 * three major activities: getting new work, processing the work, and
 * sending the reply. The loop never terminates, unless a panic occurs.
 */
  message m;					/* request message */
  int ipc_status;				/* status code */
  int call_nr, who_e,who_p;			/* call number and caller */
  int result;                 			/* result to return */

  /* SEF local startup. */
  sef_local_startup();

  /* Main loop - get work and do it, forever. */         
  while (TRUE) {              

      /* Wait for request message. */
      get_work(&m, &ipc_status);
      who_e = m.m_source;
      if(rs_isokendpt(who_e, &who_p) != OK) {
          panic("message from bogus source: %d", who_e);
      }

      call_nr = m.m_type;

      /* Now determine what to do.  Four types of requests are expected:
       * - Heartbeat messages (notifications from registered system services)
       * - System notifications (synchronous alarm)
       * - User requests (control messages to manage system services)
       * - Ready messages (reply messages from registered services)
       */

      /* Notification messages are control messages and do not need a reply.
       * These include heartbeat messages and system notifications.
       */
      if (is_ipc_notify(ipc_status)) {
          switch (who_p) {
          case CLOCK:
	      do_period(&m);			/* check services status */
	      continue;
	  default:				/* heartbeat notification */
	      if (rproc_ptr[who_p] != NULL) {	/* mark heartbeat time */ 
		  rproc_ptr[who_p]->r_alive_tm = m.NOTIFY_TIMESTAMP;
	      } else {
		  printf("RS: warning: got unexpected notify message from %d\n",
		      m.m_source);
	      }
	  }
      }

      /* If we get this far, this is a normal request.
       * Handle the request and send a reply to the caller. 
       */
      else {
	  if (call_nr != GETSYSINFO && 
	  	(call_nr < RS_RQ_BASE || call_nr >= RS_RQ_BASE+0x100))
	  {
		/* Ignore invalid requests. Do not try to reply. */
		printf("RS: warning: got invalid request %d from endpoint %d\n",
			call_nr, m.m_source);
		continue;
	  }

          /* Handler functions are responsible for permission checking. */
          switch(call_nr) {
          /* User requests. */
	  case RS_UP:		result = do_up(&m);		break;
          case RS_DOWN: 	result = do_down(&m); 		break;
          case RS_REFRESH: 	result = do_refresh(&m); 	break;
          case RS_RESTART: 	result = do_restart(&m); 	break;
          case RS_SHUTDOWN: 	result = do_shutdown(&m); 	break;
          case RS_UPDATE: 	result = do_update(&m); 	break;
          case RS_CLONE: 	result = do_clone(&m); 		break;
          case RS_EDIT: 	result = do_edit(&m); 		break;
          case GETSYSINFO: 	result = do_getsysinfo(&m); 	break;
	  case RS_LOOKUP:	result = do_lookup(&m);		break;
	  /* Ready messages. */
	  case RS_INIT: 	result = do_init_ready(&m); 	break;
	  case RS_LU_PREPARE: 	result = do_upd_ready(&m); 	break;
          default: 
              printf("RS: warning: got unexpected request %d from %d\n",
                  m.m_type, m.m_source);
              result = EINVAL;
          }

          /* Finally send reply message, unless disabled. */
          if (result != EDONTREPLY) {
	      m.m_type = result;
              reply(who_e, NULL, &m);
          }
      }
  }
}

/*===========================================================================*
 *			       sef_local_startup			     *
 *===========================================================================*/
PRIVATE void sef_local_startup()
{
  /* Register init callbacks. */
  sef_setcb_init_response(do_init_ready);
  sef_setcb_init_fresh(sef_cb_init_fresh);
  sef_setcb_init_restart(sef_cb_init_fail);

  /* Register live update callbacks. */
  sef_setcb_lu_response(do_upd_ready);

  /* Register signal callbacks. */
  sef_setcb_signal_handler(sef_cb_signal_handler);
  sef_setcb_signal_manager(sef_cb_signal_manager);

  /* Let SEF perform startup. */
  sef_startup();
}

/*===========================================================================*
 *		            sef_cb_init_fresh                                *
 *===========================================================================*/
PRIVATE int sef_cb_init_fresh(int type, sef_init_info_t *info)
{
/* Initialize the reincarnation server. */
  struct boot_image *ip;
  int s,i,j, usersched;
  int nr_image_srvs, nr_image_priv_srvs, nr_uncaught_init_srvs;
  struct rproc *rp;
  struct rproc *replica_rp;
  struct rprocpub *rpub;
  struct boot_image image[NR_BOOT_PROCS];
  struct mproc mproc[NR_PROCS];
  struct boot_image_priv *boot_image_priv;
  struct boot_image_sys *boot_image_sys;
  struct boot_image_dev *boot_image_dev;
  message m;
  int pid, replica_pid;
  endpoint_t replica_endpoint;

  /* See if we run in verbose mode. */
  env_parse("rs_verbose", "d", 0, &rs_verbose, 0, 1);

  if ((s = sys_getinfo(GET_HZ, &system_hz, sizeof(system_hz), 0, 0)) != OK)
	  panic("Cannot get system timer frequency\n");

  /* Initialize the global init descriptor. */
  rinit.rproctab_gid = cpf_grant_direct(ANY, (vir_bytes) rprocpub,
      sizeof(rprocpub), CPF_READ);
  if(!GRANT_VALID(rinit.rproctab_gid)) {
      panic("unable to create rprocpub table grant: %d", rinit.rproctab_gid);
  }

  /* Initialize some global variables. */
  rupdate.flags = 0;
  shutting_down = FALSE;

  /* Get a copy of the boot image table. */
  if ((s = sys_getimage(image)) != OK) {
      panic("unable to get copy of boot image table: %d", s);
  }

  /* Determine the number of system services in the boot image table. */
  nr_image_srvs = 0;
  for(i=0;i<NR_BOOT_PROCS;i++) {
      ip = &image[i];

      /* System services only. */
      if(iskerneln(_ENDPOINT_P(ip->endpoint))) {
          continue;
      }
      nr_image_srvs++;
  }

  /* Determine the number of entries in the boot image priv table and make sure
   * it matches the number of system services in the boot image table.
   */
  nr_image_priv_srvs = 0;
  for (i=0; boot_image_priv_table[i].endpoint != NULL_BOOT_NR; i++) {
      boot_image_priv = &boot_image_priv_table[i];

      /* System services only. */
      if(iskerneln(_ENDPOINT_P(boot_image_priv->endpoint))) {
          continue;
      }
      nr_image_priv_srvs++;
  }
  if(nr_image_srvs != nr_image_priv_srvs) {
	panic("boot image table and boot image priv table mismatch");
  }

  /* Reset the system process table. */
  for (rp=BEG_RPROC_ADDR; rp<END_RPROC_ADDR; rp++) {
      rp->r_flags = 0;
      rp->r_pub = &rprocpub[rp - rproc];
      rp->r_pub->in_use = FALSE;
  }

  /* Initialize the system process table in 4 steps, each of them following
   * the appearance of system services in the boot image priv table.
   * - Step 1: set priviliges, sys properties, and dev properties (if any)
   * for every system service.
   */
  for (i=0; boot_image_priv_table[i].endpoint != NULL_BOOT_NR; i++) {
      boot_image_priv = &boot_image_priv_table[i];

      /* System services only. */
      if(iskerneln(_ENDPOINT_P(boot_image_priv->endpoint))) {
          continue;
      }

      /* Lookup the corresponding entries in other tables. */
      boot_image_info_lookup(boot_image_priv->endpoint, image,
          &ip, NULL, &boot_image_sys, &boot_image_dev);
      rp = &rproc[boot_image_priv - boot_image_priv_table];
      rpub = rp->r_pub;

      /*
       * Set privileges.
       */
      /* Get label. */
      strcpy(rpub->label, boot_image_priv->label);

      /* Force a static priv id for system services in the boot image. */
      rp->r_priv.s_id = static_priv_id(
          _ENDPOINT_P(boot_image_priv->endpoint));
      
      /* Initialize privilege bitmaps and signal manager. */
      rp->r_priv.s_flags = boot_image_priv->flags;         /* priv flags */
      rp->r_priv.s_trap_mask = boot_image_priv->trap_mask; /* traps */
      memcpy(&rp->r_priv.s_ipc_to, &boot_image_priv->ipc_to,
                        sizeof(rp->r_priv.s_ipc_to));      /* targets */
      rp->r_priv.s_sig_mgr = boot_image_priv->sig_mgr;     /* sig mgr */
      rp->r_priv.s_bak_sig_mgr = NONE;                     /* backup sig mgr */
      
      /* Initialize kernel call mask bitmap from unordered set. */
      fill_call_mask(boot_image_priv->k_calls, NR_SYS_CALLS,
          rp->r_priv.s_k_call_mask, KERNEL_CALL, TRUE);

      /* Set the privilege structure. */
      if(boot_image_priv->endpoint != RS_PROC_NR) {
          if ((s = sys_privctl(ip->endpoint, SYS_PRIV_SET_SYS, &(rp->r_priv)))
              != OK) {
              panic("unable to set privilege structure: %d", s);
          }
      }

      /* Synch the privilege structure with the kernel. */
      if ((s = sys_getpriv(&(rp->r_priv), ip->endpoint)) != OK) {
          panic("unable to synch privilege structure: %d", s);
      }

      /*
       * Set sys properties.
       */
      rpub->sys_flags = boot_image_sys->flags;        /* sys flags */

      /*
       * Set dev properties.
       */
      rpub->dev_flags = boot_image_dev->flags;        /* device flags */
      rpub->dev_nr = boot_image_dev->dev_nr;          /* major device number */
      rpub->dev_style = boot_image_dev->dev_style;    /* device style */
      rpub->dev_style2 = boot_image_dev->dev_style2;  /* device style 2 */

      /* Get process name. */
      strcpy(rpub->proc_name, ip->proc_name);

      /* Build command settings. */
      rp->r_cmd[0]= '\0';
      rp->r_script[0]= '\0';
      build_cmd_dep(rp);

      /* Initialize vm call mask bitmap from unordered set. */
      fill_call_mask(boot_image_priv->vm_calls, NR_VM_CALLS,
          rpub->vm_call_mask, VM_RQ_BASE, TRUE);

      /* Get some settings from the boot image table. */
      rp->r_scheduler = boot_image_priv->sched;
      rp->r_priority = ip->priority;
      rp->r_quantum = ip->quantum;
      rpub->endpoint = ip->endpoint;

      /* Set some defaults. */
      rp->r_old_rp = NULL;                     /* no old version yet */
      rp->r_new_rp = NULL;                     /* no new version yet */
      rp->r_prev_rp = NULL;                    /* no prev replica yet */
      rp->r_next_rp = NULL;                    /* no next replica yet */
      rp->r_uid = 0;                           /* root */
      rp->r_check_tm = 0;                      /* not checked yet */
      getuptime(&rp->r_alive_tm);              /* currently alive */
      rp->r_stop_tm = 0;                       /* not exiting yet */
      rp->r_restarts = 0;                      /* no restarts so far */
      rp->r_set_resources = 0;                 /* don't set resources */
      rp->r_period = 0;                        /* no period yet */
      rp->r_exec = NULL;                       /* no in-memory copy yet */
      rp->r_exec_len = 0;

      /* Mark as in use and active. */
      rp->r_flags = RS_IN_USE | RS_ACTIVE;
      rproc_ptr[_ENDPOINT_P(rpub->endpoint)]= rp;
      rpub->in_use = TRUE;
  }

  /* - Step 2: allow every system service in the boot image to run.
   *           first start kernel-scheduled servers, including the PM and the
   *           scheduler which are needed to be able to start the 
   *           user-space-scheduled processes
   */
  nr_uncaught_init_srvs = 0;
  for (usersched=0; usersched <= 1; usersched++) {
  for (i=0; boot_image_priv_table[i].endpoint != NULL_BOOT_NR; i++) {
      boot_image_priv = &boot_image_priv_table[i];

      /* System services only. */
      if(iskerneln(_ENDPOINT_P(boot_image_priv->endpoint))) {
          continue;
      }

      /* Kernel-scheduled processes first */
      if ((boot_image_priv->sched == KERNEL) ? usersched : !usersched) {
          continue;
      }

      /* Lookup the corresponding slot in the system process table. */
      rp = &rproc[boot_image_priv - boot_image_priv_table];
      rpub = rp->r_pub;

      /* RS is already running as we speak. */
      if(boot_image_priv->endpoint == RS_PROC_NR) {
          if ((s = init_service(rp, SEF_INIT_FRESH)) != OK) {
              panic("unable to initialize RS: %d", s);
          }
          continue;
      }

      /* Allow the service to run. */
      if ((s = sched_init_proc(rp)) != OK) {
          panic("unable to initialize scheduling: %d", s);
      }
      if ((s = sys_privctl(rpub->endpoint, SYS_PRIV_ALLOW, NULL)) != OK) {
          panic("unable to initialize privileges: %d", s);
      }

      /* Initialize service. We assume every service will always get
       * back to us here at boot time.
       */
      if(boot_image_priv->flags & SYS_PROC) {
          if ((s = init_service(rp, SEF_INIT_FRESH)) != OK) {
              panic("unable to initialize service: %d", s);
          }
          if(rpub->sys_flags & SF_SYNCH_BOOT) {
              /* Catch init ready message now to synchronize. */
              catch_boot_init_ready(rpub->endpoint);
          }
          else {
              /* Catch init ready message later. */
              nr_uncaught_init_srvs++;
          }
      }
  }

  /* - Step 3: let every system service complete initialization by
   * catching all the init ready messages left.
   */
  while(nr_uncaught_init_srvs) {
      catch_boot_init_ready(ANY);
      nr_uncaught_init_srvs--;
  }
  }

  /* - Step 4: all the system services in the boot image are now running.
   * Complete the initialization of the system process table in collaboration
   * with other system services.
   */
  if ((s = getsysinfo(PM_PROC_NR, SI_PROC_TAB, mproc)) != OK) {
      panic("unable to get copy of PM process table: %d", s);
  }
  for (i=0; boot_image_priv_table[i].endpoint != NULL_BOOT_NR; i++) {
      boot_image_priv = &boot_image_priv_table[i];

      /* System services only. */
      if(iskerneln(_ENDPOINT_P(boot_image_priv->endpoint))) {
          continue;
      }

      /* Lookup the corresponding slot in the system process table. */
      rp = &rproc[boot_image_priv - boot_image_priv_table];
      rpub = rp->r_pub;

      /* Get pid from PM process table. */
      rp->r_pid = -1;
      for (j = 0; j < NR_PROCS; j++) {
          if (mproc[j].mp_endpoint == rpub->endpoint) {
              rp->r_pid = mproc[j].mp_pid;
              break;
          }
      }
      if(j == NR_PROCS) {
          panic("unable to get pid");
      }
  }

  /* Set alarm to periodically check service status. */
  if (OK != (s=sys_setalarm(RS_DELTA_T, 0)))
      panic("couldn't set alarm: %d", s);

  /* Now create a new RS instance with a private page table and let the current
   * instance live update into the replica. Clone RS' own slot first.
   */
  rp = rproc_ptr[_ENDPOINT_P(RS_PROC_NR)];
  if((s = clone_slot(rp, &replica_rp)) != OK) {
      panic("unable to clone current RS instance: %d", s);
  }

  /* Fork a new RS instance. */
  pid = srv_fork();
  if(pid == -1) {
      panic("unable to fork a new RS instance");
  }
  replica_pid = pid ? pid : getpid();
  replica_endpoint = getnprocnr(replica_pid);
  replica_rp->r_pid = replica_pid;
  replica_rp->r_pub->endpoint = replica_endpoint;

  if(pid == 0) {
      /* New RS instance running. */

      /* Live update the old instance into the new one. */
      s = update_service(&rp, &replica_rp, RS_SWAP);
      if(s != OK) {
          panic("unable to live update RS: %d", s);
      }
      cpf_reload();

      /* Clean up the old RS instance, the new instance will take over. */
      cleanup_service(rp);

      /* Map out our own text and data. */
      unmap_ok = 1;
      _minix_unmapzero();

      /* Ask VM to pin memory for the new RS instance. */
      if((s = vm_memctl(RS_PROC_NR, VM_RS_MEM_PIN)) != OK) {
          panic("unable to pin memory for the new RS instance: %d", s);
      }
  }
  else {
      /* Old RS instance running. */

      /* Set up privileges for the new instance and let it run. */
      s = sys_privctl(replica_endpoint, SYS_PRIV_SET_SYS, &(replica_rp->r_priv));
      if(s != OK) {
          panic("unable to set privileges for the new RS instance: %d", s);
      }
      if ((s = sched_init_proc(replica_rp)) != OK) {
          panic("unable to initialize RS replica scheduling: %d", s);
      }
      s = sys_privctl(replica_endpoint, SYS_PRIV_YIELD, NULL);
      if(s != OK) {
          panic("unable to yield control to the new RS instance: %d", s);
      }
      NOT_REACHABLE;
  }

  return(OK);
}

/*===========================================================================*
 *		            sef_cb_signal_handler                            *
 *===========================================================================*/
PRIVATE void sef_cb_signal_handler(int signo)
{
  /* Check for known signals, ignore anything else. */
  switch(signo) {
      case SIGCHLD:
          do_sigchld();
      break;
      case SIGTERM:
          do_shutdown(NULL);
      break;
  }
}

/*===========================================================================*
 *		            sef_cb_signal_manager                            *
 *===========================================================================*/
PRIVATE int sef_cb_signal_manager(endpoint_t target, int signo)
{
/* Process system signal on behalf of the kernel. */
  int target_p;
  struct rproc *rp;
  struct rprocpub *rpub;
  message m;

  /* Lookup slot. */
  if(rs_isokendpt(target, &target_p) != OK || rproc_ptr[target_p] == NULL) {
      if(rs_verbose)
          printf("RS: ignoring spurious signal %d for process %d\n",
              signo, target);
      return OK; /* clear the signal */
  }
  rp = rproc_ptr[target_p];
  rpub = rp->r_pub;

  /* Don't bother if a termination signal has already been processed. */
  if((rp->r_flags & RS_TERMINATED) && !(rp->r_flags & RS_EXITING)) {
      return EDEADEPT; /* process is gone */
  }

  /* Ignore external signals for inactive service instances. */
  if( !(rp->r_flags & RS_ACTIVE) && !(rp->r_flags & RS_EXITING)) {
      if(rs_verbose)
          printf("RS: ignoring signal %d for inactive %s\n",
              signo, srv_to_string(rp));
      return OK; /* clear the signal */
  }

  if(rs_verbose)
      printf("RS: %s got %s signal %d\n", srv_to_string(rp),
          SIGS_IS_TERMINATION(signo) ? "termination" : "non-termination",signo);

  /* Print stacktrace if necessary. */
  if(SIGS_IS_STACKTRACE(signo)) {
       sys_sysctl_stacktrace(target);
  }

  /* In case of termination signal handle the event. */
  if(SIGS_IS_TERMINATION(signo)) {
      rp->r_flags |= RS_TERMINATED;
      terminate_service(rp);

      return EDEADEPT; /* process is now gone */
  }

  /* Translate every non-termination signal into a message. */
  m.m_type = SIGS_SIGNAL_RECEIVED;
  m.SIGS_SIG_NUM = signo;
  asynsend3(rpub->endpoint, &m, AMF_NOREPLY);

  return OK; /* signal has been delivered */
}

/*===========================================================================*
 *                         boot_image_info_lookup                            *
 *===========================================================================*/
PRIVATE void boot_image_info_lookup(endpoint, image, ip, pp, sp, dp)
endpoint_t endpoint;
struct boot_image *image;
struct boot_image **ip;
struct boot_image_priv **pp;
struct boot_image_sys **sp;
struct boot_image_dev **dp;
{
/* Lookup entries in boot image tables. */
  int i;

  /* When requested, locate the corresponding entry in the boot image table
   * or panic if not found.
   */
  if(ip) {
      for (i=0; i < NR_BOOT_PROCS; i++) {
          if(image[i].endpoint == endpoint) {
              *ip = &image[i];
              break;
          }
      }
      if(i == NR_BOOT_PROCS) {
          panic("boot image table lookup failed");
      }
  }

  /* When requested, locate the corresponding entry in the boot image priv table
   * or panic if not found.
   */
  if(pp) {
      for (i=0; boot_image_priv_table[i].endpoint != NULL_BOOT_NR; i++) {
          if(boot_image_priv_table[i].endpoint == endpoint) {
              *pp = &boot_image_priv_table[i];
              break;
          }
      }
      if(i == NULL_BOOT_NR) {
          panic("boot image priv table lookup failed");
      }
  }

  /* When requested, locate the corresponding entry in the boot image sys table
   * or resort to the default entry if not found.
   */
  if(sp) {
      for (i=0; boot_image_sys_table[i].endpoint != DEFAULT_BOOT_NR; i++) {
          if(boot_image_sys_table[i].endpoint == endpoint) {
              *sp = &boot_image_sys_table[i];
              break;
          }
      }
      if(boot_image_sys_table[i].endpoint == DEFAULT_BOOT_NR) {
          *sp = &boot_image_sys_table[i];         /* accept the default entry */
      }
  }

  /* When requested, locate the corresponding entry in the boot image dev table
   * or resort to the default entry if not found.
   */
  if(dp) {
      for (i=0; boot_image_dev_table[i].endpoint != DEFAULT_BOOT_NR; i++) {
          if(boot_image_dev_table[i].endpoint == endpoint) {
              *dp = &boot_image_dev_table[i];
              break;
          }
      }
      if(boot_image_dev_table[i].endpoint == DEFAULT_BOOT_NR) {
          *dp = &boot_image_dev_table[i];         /* accept the default entry */
      }
  }
}

/*===========================================================================*
 *			      catch_boot_init_ready                          *
 *===========================================================================*/
PRIVATE void catch_boot_init_ready(endpoint)
endpoint_t endpoint;
{
/* Block and catch an init ready message from the given source. */
  int r;
  int ipc_status;
  message m;
  struct rproc *rp;
  int result;

  /* Receive init ready message. */
  if ((r = sef_receive_status(endpoint, &m, &ipc_status)) != OK) {
      panic("unable to receive init reply: %d", r);
  }
  if(m.m_type != RS_INIT) {
      panic("unexpected reply from service: %d", m.m_source);
  }
  result = m.RS_INIT_RESULT;
  rp = rproc_ptr[_ENDPOINT_P(m.m_source)];

  /* Check result. */
  if(result != OK) {
      panic("unable to complete init for service: %d", m.m_source);
  }

  /* Send a reply to unblock the service. */
  m.m_type = OK;
  reply(m.m_source, rp, &m);

  /* Mark the slot as no longer initializing. */
  rp->r_flags &= ~RS_INITIALIZING;
  rp->r_check_tm = 0;
  getuptime(&rp->r_alive_tm);
}

/*===========================================================================*
 *				get_work                                     *
 *===========================================================================*/
PRIVATE void get_work(m_ptr, status_ptr)
message *m_ptr;				/* pointer to message */
int *status_ptr;			/* pointer to status */
{
    int r;
    if (OK != (r=sef_receive_status(ANY, m_ptr, status_ptr)))
        panic("sef_receive_status failed: %d", r);
}

