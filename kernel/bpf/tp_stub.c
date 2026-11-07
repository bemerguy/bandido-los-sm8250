// SPDX-License-Identifier: GPL-2.0
/*
 * Loadable-only BPF_PROG_TYPE_TRACEPOINT for kernels built without
 * CONFIG_BPF_EVENTS. Android's bpfloader must be able to load tracepoint
 * programs (e.g. gpuMem.o) so their maps get pinned; libmeminfo on
 * Android 16 aborts system_server when map_gpuMem_gpu_mem_total_map is
 * missing. Programs load but can never be attached, as no tracepoints exist.
 */
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/trace_events.h>

static const struct bpf_func_proto *
tp_stub_func_proto(enum bpf_func_id func_id, const struct bpf_prog *prog)
{
	return bpf_base_func_proto(func_id);
}

static bool tp_stub_is_valid_access(int off, int size, enum bpf_access_type type,
				    const struct bpf_prog *prog,
				    struct bpf_insn_access_aux *info)
{
	if (off < sizeof(void *) || off >= PERF_MAX_TRACE_SIZE)
		return false;
	if (type != BPF_READ)
		return false;
	if (off % size != 0)
		return false;
	return true;
}

const struct bpf_verifier_ops tracepoint_verifier_ops = {
	.get_func_proto  = tp_stub_func_proto,
	.is_valid_access = tp_stub_is_valid_access,
};

const struct bpf_prog_ops tracepoint_prog_ops = {
};
