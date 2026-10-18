/*
 * Vendor Gunyah symbol declarations needed by the pre-compile check.
 *
 * The real vendor headers live in tools/src/crdroid, which is gitignored
 * because it is reference material rather than project source. The check
 * script needs to see the vendor declarations to tell a genuine typo from
 * a symbol it simply cannot resolve, so the declarations it uses are
 * extracted here.
 *
 * Regenerate with:  pwsh scripts/gen_vendor_decls.ps1
 *
 * This file is data for the checker only. It is never compiled.
 */

#ifndef _GH_VENDOR_DECLS_H
#define _GH_VENDOR_DECLS_H

/* ---- include/linux/gunyah/*.h and gh_rm_drv_private.h ---- */

int gh_arch_validate_vm_exited_notif(void);
int gh_get_image_name(void);
int gh_get_irq(void);
int gh_get_virq(void);
int gh_get_vm_name(void);
int gh_init_vm_prop_table(void);
int gh_irq_accept(void);
int gh_irq_accept_notify(void);
int gh_irq_lend(void);
int gh_irq_lend_notify(void);
int gh_irq_lend_v2(void);
int gh_irq_reclaim(void);
int gh_irq_release(void);
int gh_irq_release_notify(void);
int gh_irq_wait_for_lend(void);
int gh_irq_wait_for_lend_v2(void);
int gh_mem_notifier_register(void);
int gh_mem_notifier_unregister(void);
int gh_msgq_populate_cap_info(void);
int gh_msgq_probe(void);
int gh_msgq_recv(void);
int gh_msgq_register(void);
int gh_msgq_reset_cap_info(void);
int gh_msgq_send(void);
int gh_msgq_unregister(void);
int gh_put_irq(void);
int gh_put_virq(void);
int gh_rm_call(void);
int gh_rm_console_close(void);
int gh_rm_console_flush(void);
int gh_rm_console_open(void);
int gh_rm_console_write(void);
int gh_rm_get_vm_id_info(void);
int gh_rm_get_vm_name(void);
int gh_rm_get_vmid(void);
int gh_rm_get_vminfo(void);
int gh_rm_irq_to_virq(void);
int gh_rm_mem_accept(void);
int gh_rm_mem_donate(void);
int gh_rm_mem_lend(void);
int gh_rm_mem_notify(void);
int gh_rm_mem_qcom_lookup_sgl(void);
int gh_rm_mem_reclaim(void);
int gh_rm_mem_release(void);
int gh_rm_mem_share(void);
int gh_rm_populate_hyp_res(void);
int gh_rm_register_notifier(void);
int gh_rm_reset_vcpu_affinity_cb(void);
int gh_rm_reset_vpm_grp_cb(void);
int gh_rm_set_vcpu_affinity_cb(void);
int gh_rm_set_virtio_mmio_cb(void);
int gh_rm_set_vpm_grp_cb(void);
int gh_rm_unpopulate_hyp_res(void);
int gh_rm_unregister_notifier(void);
int gh_rm_unset_virtio_mmio_cb(void);
int gh_rm_virq_to_irq(void);
int gh_rm_vm_alloc_vmid(void);
int gh_rm_vm_dealloc_vmid(void);
int gh_rm_vm_get_hyp_res(void);
int gh_rm_vm_get_id(void);
int gh_rm_vm_get_status(void);
int gh_rm_vm_irq_accept(void);
int gh_rm_vm_irq_accept_notify(void);
int gh_rm_vm_irq_lend(void);
int gh_rm_vm_irq_lend_notify(void);
int gh_rm_vm_irq_reclaim(void);
int gh_rm_vm_irq_release(void);
int gh_rm_vm_irq_release_notify(void);
int gh_rm_vm_lookup(void);
int gh_rm_vm_reset(void);
int gh_rm_vm_set_app_status(void);
int gh_rm_vm_set_os_status(void);
int gh_rm_vm_set_status(void);
int gh_rm_vm_set_vm_status(void);
int gh_rm_vm_start(void);
int gh_rm_vm_stop(void);
int gh_update_vm_prop_table(void);

#endif
/* vendor-tag: enum gh_irq_label */
/* vendor-tag: enum gh_mem_notifier_tag */
/* vendor-tag: enum gh_msgq_label */
/* vendor-tag: enum gh_vm_lookup_type */
/* vendor-tag: enum gh_vm_names */
/* vendor-tag: struct __packed */
/* vendor-tag: struct gh_acl_desc */
/* vendor-tag: struct gh_acl_entry */
/* vendor-tag: struct gh_mem_accept_req_payload_hdr */
/* vendor-tag: struct gh_mem_accept_resp_payload */
/* vendor-tag: struct gh_mem_attr_desc */
/* vendor-tag: struct gh_mem_attr_entry */
/* vendor-tag: struct gh_mem_notify_req_payload */
/* vendor-tag: struct gh_mem_qcom_lookup_sgl_req_payload_hdr */
/* vendor-tag: struct gh_mem_qcom_lookup_sgl_resp_payload */
/* vendor-tag: struct gh_mem_release_req_payload */
/* vendor-tag: struct gh_mem_share_req_payload_hdr */
/* vendor-tag: struct gh_mem_share_resp_payload */
/* vendor-tag: struct gh_notify_vmid_desc */
/* vendor-tag: struct gh_notify_vmid_entry */
/* vendor-tag: struct gh_rm_mem_shared_acl_entry */
/* vendor-tag: struct gh_rm_mem_shared_attr_entry */
/* vendor-tag: struct gh_rm_mem_shared_sgl_entry */
/* vendor-tag: struct gh_rm_notif_mem_accepted_payload */
/* vendor-tag: struct gh_rm_notif_mem_released_payload */
/* vendor-tag: struct gh_rm_notif_mem_shared_payload */
/* vendor-tag: struct gh_rm_notif_vm_console_chars */
/* vendor-tag: struct gh_rm_notif_vm_exited_payload */
/* vendor-tag: struct gh_rm_notif_vm_irq_accepted_payload */
/* vendor-tag: struct gh_rm_notif_vm_irq_lent_payload */
/* vendor-tag: struct gh_rm_notif_vm_irq_released_payload */
/* vendor-tag: struct gh_rm_notif_vm_shutdown_payload */
/* vendor-tag: struct gh_rm_notif_vm_status_payload */
/* vendor-tag: struct gh_rm_rpc_hdr */
/* vendor-tag: struct gh_rm_rpc_reply_hdr */
/* vendor-tag: struct gh_sgl_desc */
/* vendor-tag: struct gh_sgl_entry */
/* vendor-tag: struct gh_vm_allocate_req_payload */
/* vendor-tag: struct gh_vm_allocate_resp_payload */
/* vendor-tag: struct gh_vm_console_common_req_payload */
/* vendor-tag: struct gh_vm_console_write_req_payload */
/* vendor-tag: struct gh_vm_deallocate_req_payload */
/* vendor-tag: struct gh_vm_exit_reason_vm_exit */
/* vendor-tag: struct gh_vm_get_hyp_res_req_payload */
/* vendor-tag: struct gh_vm_get_hyp_res_resp_entry */
/* vendor-tag: struct gh_vm_get_hyp_res_resp_payload */
/* vendor-tag: struct gh_vm_get_id_req_payload */
/* vendor-tag: struct gh_vm_get_id_resp_entry */
/* vendor-tag: struct gh_vm_get_id_resp_payload */
/* vendor-tag: struct gh_vm_get_state_req_payload */
/* vendor-tag: struct gh_vm_get_state_resp_payload */
/* vendor-tag: struct gh_vm_irq_accept_req_payload */
/* vendor-tag: struct gh_vm_irq_accept_resp_payload */
/* vendor-tag: struct gh_vm_irq_lend_req_payload */
/* vendor-tag: struct gh_vm_irq_lend_resp_payload */
/* vendor-tag: struct gh_vm_irq_notify_req_payload */
/* vendor-tag: struct gh_vm_irq_reclaim_req_payload */
/* vendor-tag: struct gh_vm_irq_release_req_payload */
/* vendor-tag: struct gh_vm_lookup_char_req_payload */
/* vendor-tag: struct gh_vm_lookup_resp_entry */
/* vendor-tag: struct gh_vm_lookup_resp_payload */
/* vendor-tag: struct gh_vm_property */
/* vendor-tag: struct gh_vm_reset_req_payload */
/* vendor-tag: struct gh_vm_set_status_req_payload */
/* vendor-tag: struct gh_vm_start_req_payload */
/* vendor-tag: struct gh_vm_start_resp_payload */
/* vendor-tag: struct gh_vm_status */
/* vendor-tag: struct gh_vm_stop_req_payload */
/* vendor-tag: struct gh_vminfo */
