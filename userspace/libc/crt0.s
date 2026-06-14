# C runtime startup -- _start -> main -> exit
# The kernel sets up a clean ring-3 environment with RSP pointing
# to the user stack.  We zero the frame pointer, call main(argc, argv),
# then pass the return value to exit().

.text
.globl _start
.type _start, @function
_start:
    # Clear base pointer for clean backtraces
    xorq    %rbp, %rbp

    # argc is at [rsp], argv = rsp + 8
    movq    (%rsp), %rdi        # arg1 = argc
    leaq    8(%rsp), %rsi       # arg2 = argv

    # Align stack to 16 bytes (rsp+8 is 16-byte aligned after push of argc)
    andq    $-16, %rsp
    pushq   %rax                # push to misalign for call (ret will pop)

    call    main

    # exit(main_return_value)
    movl    %eax, %edi
    call    exit

    # Should never reach here
    hlt
.size _start, . - _start
