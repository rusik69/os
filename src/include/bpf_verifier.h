#ifndef BPF_VERIFIER_H
#define BPF_VERIFIER_H

#include "types.h"

/* Verify an eBPF program.
 * @prog:     pointer to eBPF instruction array
 * @insn_cnt: number of instructions in the program
 * @prog_type: type of program (BPF_PROG_TYPE_*)
 * @log:      optional log buffer for verbose verification output
 * @log_size: size of log buffer
 *
 * Returns 0 on success (program is safe), negative errno on failure.
 */
int bpf_verify_program(const struct bpf_insn *prog, int insn_cnt,
                       int prog_type, char *log, int log_size);

/* Initialize the BPF verifier subsystem */
int bpf_verify_init(void);

#endif /* BPF_VERIFIER_H */
