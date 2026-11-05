; Player-chat call site of ChatSystem::addMessage in processIncomingMessages (0x7b45b0), call at 0x7b50d9, return 0x7b50de
; plus the session lookup 0x9f7600 it calls at 0x7b5037. Same exe as 1273043-addMessage.asm. See ../enshrouded.md

; --- processIncomingMessages 0x7b4f1f..0x7b5182 ---
007b4f1f  498b95d0010000           mov rdx, qword ptr [r13 + 0x1d0]
007b4f26  e8252fe9ff               call 0x647e50
007b4f2b  488b08                   mov rcx, qword ptr [rax]
007b4f2e  4885c9                   test rcx, rcx
007b4f31  0f84a6020000             je 0x7b51dd
007b4f37  8b8574090000             mov eax, dword ptr [rbp + 0x974]
007b4f3d  8901                     mov dword ptr [rcx], eax
007b4f3f  e999020000               jmp 0x7b51dd
007b4f44  488b4598                 mov rax, qword ptr [rbp - 0x68]
007b4f48  4c8d05e9b8ce00           lea r8, [rip + 0xceb8e9]
007b4f4f  4889442458               mov qword ptr [rsp + 0x58], rax
007b4f54  488d542450               lea rdx, [rsp + 0x50]
007b4f59  488b45a0                 mov rax, qword ptr [rbp - 0x60]
007b4f5d  488d8d10050000           lea rcx, [rbp + 0x510]
007b4f64  89bd70090000             mov dword ptr [rbp + 0x970], edi
007b4f6a  4c89642450               mov qword ptr [rsp + 0x50], r12
007b4f6f  4c89642468               mov qword ptr [rsp + 0x68], r12
007b4f74  488d04c500000000         lea rax, [rax*8]
007b4f7c  4c89642470               mov qword ptr [rsp + 0x70], r12
007b4f81  4889442460               mov qword ptr [rsp + 0x60], rax
007b4f86  c644247800               mov byte ptr [rsp + 0x78], 0
007b4f8b  e8107a1e00               call 0x99c9a0
007b4f90  84c0                     test al, al
007b4f92  0f8446010000             je 0x7b50de
007b4f98  448b8570090000           mov r8d, dword ptr [rbp + 0x970]
007b4f9f  488d8518050000           lea rax, [rbp + 0x518]
007b4fa6  498b5510                 mov rdx, qword ptr [r13 + 0x10]
007b4faa  488d8d00010000           lea rcx, [rbp + 0x100]
007b4fb1  488945f8                 mov qword ptr [rbp - 8], rax
007b4fb5  498bdc                   mov rbx, r12
007b4fb8  8b8514050000             mov eax, dword ptr [rbp + 0x514]
007b4fbe  48894500                 mov qword ptr [rbp], rax
007b4fc2  33c0                     xor eax, eax
007b4fc4  48898500010000           mov qword ptr [rbp + 0x100], rax
007b4fcb  8b0573c4d100             mov eax, dword ptr [rip + 0xd1c473]
007b4fd1  898508010000             mov dword ptr [rbp + 0x108], eax
007b4fd7  33c0                     xor eax, eax
007b4fd9  89850c010000             mov dword ptr [rbp + 0x10c], eax
007b4fdf  888510010000             mov byte ptr [rbp + 0x110], al
007b4fe5  48898520010000           mov qword ptr [rbp + 0x120], rax
007b4fec  488d8548010000           lea rax, [rbp + 0x148]
007b4ff3  48898528010000           mov qword ptr [rbp + 0x128], rax
007b4ffa  33c0                     xor eax, eax
007b4ffc  88854c020000             mov byte ptr [rbp + 0x24c], al
007b5002  88854e020000             mov byte ptr [rbp + 0x24e], al
007b5008  4c89a518010000           mov qword ptr [rbp + 0x118], r12
007b500f  4c89a530010000           mov qword ptr [rbp + 0x130], r12
007b5016  48c7853801000000010000   mov qword ptr [rbp + 0x138], 0x100
007b5021  4c89a540010000           mov qword ptr [rbp + 0x140], r12
007b5028  c6854802000000           mov byte ptr [rbp + 0x248], 0
007b502f  664489a54a020000         mov word ptr [rbp + 0x24a], r12w
007b5037  e8c4252400               call 0x9f7600
007b503c  84c0                     test al, al
007b503e  7509                     jne 0x7b5049
007b5040  488b9d20010000           mov rbx, qword ptr [rbp + 0x120]
007b5047  eb4c                     jmp 0x7b5095
007b5049  0f100590d3b500           movups xmm0, xmmword ptr [rip + 0xb5d390]
007b5050  8b452c                   mov eax, dword ptr [rbp + 0x2c]
007b5053  4c8d45a8                 lea r8, [rbp - 0x58]
007b5057  8945ac                   mov dword ptr [rbp - 0x54], eax
007b505a  488d154fdbb500           lea rdx, [rip + 0xb5db4f]
007b5061  0f1145c0                 movups xmmword ptr [rbp - 0x40], xmm0
007b5065  488d8570090000           lea rax, [rbp + 0x970]
007b506c  41b901000000             mov r9d, 1
007b5072  f20f100576d3b500         movsd xmm0, qword ptr [rip + 0xb5d376]
007b507a  b102                     mov cl, 2
007b507c  f20f1145d0               movsd qword ptr [rbp - 0x30], xmm0
007b5081  c745a827000000           mov dword ptr [rbp - 0x58], 0x27
007b5088  4c897db0                 mov qword ptr [rbp - 0x50], r15
007b508c  488945b8                 mov qword ptr [rbp - 0x48], rax
007b5090  e88baddbff               call 0x56fe20
007b5095  440fb68510050000         movzx r8d, byte ptr [rbp + 0x510]
007b509d  488d4518                 lea rax, [rbp + 0x18]
007b50a1  498b5560                 mov rdx, qword ptr [r13 + 0x60]
007b50a5  4c8d4df8                 lea r9, [rbp - 8]
007b50a9  4889442440               mov qword ptr [rsp + 0x40], rax
007b50ae  488d4d08                 lea rcx, [rbp + 8]
007b50b2  8b8570090000             mov eax, dword ptr [rbp + 0x970]
007b50b8  48895c2438               mov qword ptr [rsp + 0x38], rbx
007b50bd  89442430                 mov dword ptr [rsp + 0x30], eax
007b50c1  8b851c090000             mov eax, dword ptr [rbp + 0x91c]
007b50c7  89442428                 mov dword ptr [rsp + 0x28], eax
007b50cb  8b8518090000             mov eax, dword ptr [rbp + 0x918]
007b50d1  89442420                 mov dword ptr [rsp + 0x20], eax
007b50d5  c6452000                 mov byte ptr [rbp + 0x20], 0
007b50d9  e852621500               call 0x90b330
007b50de  488b4c2450               mov rcx, qword ptr [rsp + 0x50]
007b50e3  4885c9                   test rcx, rcx
007b50e6  0f84f1000000             je 0x7b51dd
007b50ec  807c247800               cmp byte ptr [rsp + 0x78], 0
007b50f1  0f85e6000000             jne 0x7b51dd
007b50f7  488b442468               mov rax, qword ptr [rsp + 0x68]
007b50fc  48894110                 mov qword ptr [rcx + 0x10], rax
007b5100  e9d8000000               jmp 0x7b51dd
007b5105  498b8d98160000           mov rcx, qword ptr [r13 + 0x1698]
007b510c  4c8d442450               lea r8, [rsp + 0x50]
007b5111  410fb6d6                 movzx edx, r14b
007b5115  0f29442450               movaps xmmword ptr [rsp + 0x50], xmm0
007b511a  e871b92500               call 0xa10a90
007b511f  e9b9000000               jmp 0x7b51dd
007b5124  498b8dd0010000           mov rcx, qword ptr [r13 + 0x1d0]
007b512b  e840d9e9ff               call 0x652a70
007b5130  498b8da0010000           mov rcx, qword ptr [r13 + 0x1a0]
007b5137  488d5598                 lea rdx, [rbp - 0x68]
007b513b  4d8bce                   mov r9, r14
007b513e  448bc0                   mov r8d, eax
007b5141  e80a7da8ff               call 0x23ce50
007b5146  e992000000               jmp 0x7b51dd
007b514b  b9af86c818               mov ecx, 0x18c886af
007b5150  e87b2d0200               call 0x7d7ed0
007b5155  448bc6                   mov r8d, esi
007b5158  488d4de8                 lea rcx, [rbp - 0x18]
007b515c  488bd0                   mov rdx, rax
007b515f  e8ec320200               call 0x7d8450
007b5164  0f100575d2b500           movups xmm0, xmmword ptr [rip + 0xb5d275]
007b516b  488b45e8                 mov rax, qword ptr [rbp - 0x18]
007b516f  4c8d45a8                 lea r8, [rbp - 0x58]
007b5173  488945d8                 mov qword ptr [rbp - 0x28], rax
007b5177  488d15cad8b500           lea rdx, [rip + 0xb5d8ca]
007b517e  488b45f0                 mov rax, qword ptr [rbp - 0x10]
007b5182  41b901000000             mov r9d, 1

; --- session lookup 0x9f7600 ---
root ['0x9f7600', '0x9f7684', '0x147ab64']
--- chunk 9f7600-9f7684 unwind 147ab64 flags 0
009f7600  4053                     push rbx
009f7602  4883ec20                 sub rsp, 0x20
009f7606  8b423c                   mov eax, dword ptr [rdx + 0x3c]
009f7609  4c8d5240                 lea r10, [rdx + 0x40]
009f760d  90                       nop 
009f760e  f041ff0482               lock inc dword ptr [r10 + rax*4]
009f7613  4c69c868260000           imul r9, rax, 0x2668
009f761a  418bc0                   mov eax, r8d
009f761d  83e03f                   and eax, 0x3f
009f7620  4983c148                 add r9, 0x48
009f7624  4c69d850010000           imul r11, rax, 0x150
009f762b  4c03ca                   add r9, rdx
009f762e  48b8df248bf41239a66a     movabs rax, 0x6aa63912f48b24df
009f7638  4d03d9                   add r11, r9
009f763b  4c2bca                   sub r9, rdx
009f763e  4983e948                 sub r9, 0x48
009f7642  49f7e9                   imul r9
009f7645  48c1fa0c                 sar rdx, 0xc
009f7649  488bc2                   mov rax, rdx
009f764c  48c1e83f                 shr rax, 0x3f
009f7650  4803c2                   add rax, rdx
009f7653  8bc0                     mov eax, eax
009f7655  498d1c82                 lea rbx, [r10 + rax*4]
009f7659  45398378010000           cmp dword ptr [r11 + 0x178], r8d
009f7660  740b                     je 0x9f766d
009f7662  f0ff0b                   lock dec dword ptr [rbx]
009f7665  b01f                     mov al, 0x1f
009f7667  4883c420                 add rsp, 0x20
009f766b  5b                       pop rbx
009f766c  c3                       ret 
009f766d  498d9378010000           lea rdx, [r11 + 0x178]
009f7674  e8875bffff               call 0x9ed200
009f7679  f0ff0b                   lock dec dword ptr [rbx]
009f767c  32c0                     xor al, al
009f767e  4883c420                 add rsp, 0x20
009f7682  5b                       pop rbx
009f7683  c3                       ret 
