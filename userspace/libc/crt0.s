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

    # Align stack to 16 bytes before call.  The call instruction pushes
    # the 8-byte return address, so RSP % 16 == 8 at main entry (ABI).
    andq    $-16, %rsp

    call    main

    # exit(main_return_value)
    movl    %eax, %edi
    call    exit

    # Should never reach here
    hlt
.size _start, . - _start

# Mark stack as non-executable
.section .note.GNU-stack, "", @note
