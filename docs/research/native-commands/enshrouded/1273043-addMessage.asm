; ChatSystem::addMessage, Enshrouded dedicated server game build 1273043 (Steam build 26083076)
; enshrouded_server.exe sha256 d7baa768eb00aa895d9f5a1cd8747ccf66858c7b542f50983092b2e3261588ee, ImageBase 0x140000000, addresses are RVAs
; capstone 5.0.7; function extent from .pdata (three chunks, chained unwind info followed). See ../enshrouded.md
root ['0x90b330', '0x90b36e', '0x1465b00']
--- chunk 90b330-90b36e unwind 1465b00 flags 0
0090b330  4055                     push rbp
0090b332  56                       push rsi
0090b333  4154                     push r12
0090b335  4155                     push r13
0090b337  4157                     push r15
0090b339  4883ec30                 sub rsp, 0x30
0090b33d  498b4108                 mov rax, qword ptr [r9 + 8]
0090b341  33ed                     xor ebp, ebp
0090b343  4d8bf9                   mov r15, r9
0090b346  450fb6e0                 movzx r12d, r8b
0090b34a  488bf2                   mov rsi, rdx
0090b34d  4c8be9                   mov r13, rcx
0090b350  483d00040000             cmp rax, 0x400
0090b356  7616                     jbe 0x90b36e
0090b358  896904                   mov dword ptr [rcx + 4], ebp
0090b35b  488bc1                   mov rax, rcx
0090b35e  c60112                   mov byte ptr [rcx], 0x12
0090b361  4883c430                 add rsp, 0x30
0090b365  415f                     pop r15
0090b367  415d                     pop r13
0090b369  415c                     pop r12
0090b36b  5e                       pop rsi
0090b36c  5d                       pop rbp
0090b36d  c3                       ret 
--- chunk 90b36e-90b478 unwind 1465b10 flags 4
0090b36e  48895c2460               mov qword ptr [rsp + 0x60], rbx
0090b373  48897c2468               mov qword ptr [rsp + 0x68], rdi
0090b378  4c89742470               mov qword ptr [rsp + 0x70], r14
0090b37d  4c8d7040                 lea r14, [rax + 0x40]
0090b381  498bd6                   mov rdx, r14
0090b384  488d8e90010000           lea rcx, [rsi + 0x190]
0090b38b  e8a050c3ff               call 0x540430
0090b390  488bf8                   mov rdi, rax
0090b393  4885c0                   test rax, rax
0090b396  7572                     jne 0x90b40a
0090b398  488b8ec0010000           mov rcx, qword ptr [rsi + 0x1c0]
0090b39f  4885c9                   test rcx, rcx
0090b3a2  7505                     jne 0x90b3a9
0090b3a4  488bcd                   mov rcx, rbp
0090b3a7  eb3a                     jmp 0x90b3e3
0090b3a9  488b4130                 mov rax, qword ptr [rcx + 0x30]
0090b3ad  4885c0                   test rax, rax
0090b3b0  7411                     je 0x90b3c3
0090b3b2  48896828                 mov qword ptr [rax + 0x28], rbp
0090b3b6  488b86c0010000           mov rax, qword ptr [rsi + 0x1c0]
0090b3bd  488b5030                 mov rdx, qword ptr [rax + 0x30]
0090b3c1  eb0a                     jmp 0x90b3cd
0090b3c3  4889aec8010000           mov qword ptr [rsi + 0x1c8], rbp
0090b3ca  488bd5                   mov rdx, rbp
0090b3cd  488996c0010000           mov qword ptr [rsi + 0x1c0], rdx
0090b3d4  48896930                 mov qword ptr [rcx + 0x30], rbp
0090b3d8  48896928                 mov qword ptr [rcx + 0x28], rbp
0090b3dc  48ff8ed0010000           dec qword ptr [rsi + 0x1d0]
0090b3e3  8b4108                   mov eax, dword ptr [rcx + 8]
0090b3e6  488d542420               lea rdx, [rsp + 0x20]
0090b3eb  4883c040                 add rax, 0x40
0090b3ef  48894c2420               mov qword ptr [rsp + 0x20], rcx
0090b3f4  488d8e90010000           lea rcx, [rsi + 0x190]
0090b3fb  4889442428               mov qword ptr [rsp + 0x28], rax
0090b400  e88bb5c4ff               call 0x556990
0090b405  e977ffffff               jmp 0x90b381
0090b40a  4d8bc6                   mov r8, r14
0090b40d  33d2                     xor edx, edx
0090b40f  488bcf                   mov rcx, rdi
0090b412  e829a82b00               call 0xbc5c40
0090b417  4d8b4708                 mov r8, qword ptr [r15 + 8]
0090b41b  488d5f40                 lea rbx, [rdi + 0x40]
0090b41f  498b17                   mov rdx, qword ptr [r15]
0090b422  488bcb                   mov rcx, rbx
0090b425  e876a12b00               call 0xbc55a0
0090b42a  8b842490000000           mov eax, dword ptr [rsp + 0x90]
0090b431  48f7db                   neg rbx
0090b434  894714                   mov dword ptr [rdi + 0x14], eax
0090b437  1bc0                     sbb eax, eax
0090b439  448827                   mov byte ptr [rdi], r12b
0090b43c  83e03c                   and eax, 0x3c
0090b43f  896f20                   mov dword ptr [rdi + 0x20], ebp
0090b442  498b4f08                 mov rcx, qword ptr [r15 + 8]
0090b446  894704                   mov dword ptr [rdi + 4], eax
0090b449  8b842480000000           mov eax, dword ptr [rsp + 0x80]
0090b450  4c8b742470               mov r14, qword ptr [rsp + 0x70]
0090b455  488b5c2460               mov rbx, qword ptr [rsp + 0x60]
0090b45a  894f08                   mov dword ptr [rdi + 8], ecx
0090b45d  89470c                   mov dword ptr [rdi + 0xc], eax
0090b460  8b842488000000           mov eax, dword ptr [rsp + 0x88]
0090b467  894710                   mov dword ptr [rdi + 0x10], eax
0090b46a  488b8424a0000000         mov rax, qword ptr [rsp + 0xa0]
0090b472  40386808                 cmp byte ptr [rax + 8], bpl
0090b476  7405                     je 0x90b47d
--- chunk 90b478-90b6af unwind 1465b2c flags 4
0090b478  488b00                   mov rax, qword ptr [rax]
0090b47b  eb03                     jmp 0x90b480
0090b47d  488bc5                   mov rax, rbp
0090b480  48894738                 mov qword ptr [rdi + 0x38], rax
0090b484  488b842498000000         mov rax, qword ptr [rsp + 0x98]
0090b48c  48894718                 mov qword ptr [rdi + 0x18], rax
0090b490  488b86c8010000           mov rax, qword ptr [rsi + 0x1c8]
0090b497  4885c0                   test rax, rax
0090b49a  7509                     jne 0x90b4a5
0090b49c  4889bec0010000           mov qword ptr [rsi + 0x1c0], rdi
0090b4a3  eb0f                     jmp 0x90b4b4
0090b4a5  48897830                 mov qword ptr [rax + 0x30], rdi
0090b4a9  488b86c8010000           mov rax, qword ptr [rsi + 0x1c8]
0090b4b0  48894728                 mov qword ptr [rdi + 0x28], rax
0090b4b4  4889bec8010000           mov qword ptr [rsi + 0x1c8], rdi
0090b4bb  48ff86d0010000           inc qword ptr [rsi + 0x1d0]
0090b4c2  448b4708                 mov r8d, dword ptr [rdi + 8]
0090b4c6  4983c017                 add r8, 0x17
0090b4ca  498bc0                   mov rax, r8
0090b4cd  83e003                   and eax, 3
0090b4d0  7407                     je 0x90b4d9
0090b4d2  4c2bc0                   sub r8, rax
0090b4d5  4983c004                 add r8, 4
0090b4d9  4c8bd5                   mov r10, rbp
0090b4dc  488d462c                 lea rax, [rsi + 0x2c]
0090b4e0  458bca                   mov r9d, r10d
0090b4e3  403868e0                 cmp byte ptr [rax - 0x20], bpl
0090b4e7  750c                     jne 0x90b4f5
0090b4e9  8b4f20                   mov ecx, dword ptr [rdi + 0x20]
0090b4ec  440fabd1                 bts ecx, r10d
0090b4f0  894f20                   mov dword ptr [rdi + 0x20], ecx
0090b4f3  eb04                     jmp 0x90b4f9
0090b4f5  440140e8                 add dword ptr [rax - 0x18], r8d
0090b4f9  403868ec                 cmp byte ptr [rax - 0x14], bpl
0090b4fd  750f                     jne 0x90b50e
0090b4ff  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b502  418d4901                 lea ecx, [r9 + 1]
0090b506  0fabca                   bts edx, ecx
0090b509  895720                   mov dword ptr [rdi + 0x20], edx
0090b50c  eb04                     jmp 0x90b512
0090b50e  440140f4                 add dword ptr [rax - 0xc], r8d
0090b512  403868f8                 cmp byte ptr [rax - 8], bpl
0090b516  750f                     jne 0x90b527
0090b518  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b51b  418d4902                 lea ecx, [r9 + 2]
0090b51f  0fabca                   bts edx, ecx
0090b522  895720                   mov dword ptr [rdi + 0x20], edx
0090b525  eb03                     jmp 0x90b52a
0090b527  440100                   add dword ptr [rax], r8d
0090b52a  40386804                 cmp byte ptr [rax + 4], bpl
0090b52e  750f                     jne 0x90b53f
0090b530  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b533  418d4903                 lea ecx, [r9 + 3]
0090b537  0fabca                   bts edx, ecx
0090b53a  895720                   mov dword ptr [rdi + 0x20], edx
0090b53d  eb04                     jmp 0x90b543
0090b53f  4401400c                 add dword ptr [rax + 0xc], r8d
0090b543  40386810                 cmp byte ptr [rax + 0x10], bpl
0090b547  750f                     jne 0x90b558
0090b549  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b54c  418d4904                 lea ecx, [r9 + 4]
0090b550  0fabca                   bts edx, ecx
0090b553  895720                   mov dword ptr [rdi + 0x20], edx
0090b556  eb04                     jmp 0x90b55c
0090b558  44014018                 add dword ptr [rax + 0x18], r8d
0090b55c  4038681c                 cmp byte ptr [rax + 0x1c], bpl
0090b560  750f                     jne 0x90b571
0090b562  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b565  418d4905                 lea ecx, [r9 + 5]
0090b569  0fabca                   bts edx, ecx
0090b56c  895720                   mov dword ptr [rdi + 0x20], edx
0090b56f  eb04                     jmp 0x90b575
0090b571  44014024                 add dword ptr [rax + 0x24], r8d
0090b575  40386828                 cmp byte ptr [rax + 0x28], bpl
0090b579  750f                     jne 0x90b58a
0090b57b  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b57e  418d4906                 lea ecx, [r9 + 6]
0090b582  0fabca                   bts edx, ecx
0090b585  895720                   mov dword ptr [rdi + 0x20], edx
0090b588  eb04                     jmp 0x90b58e
0090b58a  44014030                 add dword ptr [rax + 0x30], r8d
0090b58e  40386834                 cmp byte ptr [rax + 0x34], bpl
0090b592  750f                     jne 0x90b5a3
0090b594  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b597  418d4907                 lea ecx, [r9 + 7]
0090b59b  0fabca                   bts edx, ecx
0090b59e  895720                   mov dword ptr [rdi + 0x20], edx
0090b5a1  eb04                     jmp 0x90b5a7
0090b5a3  4401403c                 add dword ptr [rax + 0x3c], r8d
0090b5a7  40386840                 cmp byte ptr [rax + 0x40], bpl
0090b5ab  750f                     jne 0x90b5bc
0090b5ad  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b5b0  418d4908                 lea ecx, [r9 + 8]
0090b5b4  0fabca                   bts edx, ecx
0090b5b7  895720                   mov dword ptr [rdi + 0x20], edx
0090b5ba  eb04                     jmp 0x90b5c0
0090b5bc  44014048                 add dword ptr [rax + 0x48], r8d
0090b5c0  4038684c                 cmp byte ptr [rax + 0x4c], bpl
0090b5c4  750f                     jne 0x90b5d5
0090b5c6  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b5c9  418d4909                 lea ecx, [r9 + 9]
0090b5cd  0fabca                   bts edx, ecx
0090b5d0  895720                   mov dword ptr [rdi + 0x20], edx
0090b5d3  eb04                     jmp 0x90b5d9
0090b5d5  44014054                 add dword ptr [rax + 0x54], r8d
0090b5d9  40386858                 cmp byte ptr [rax + 0x58], bpl
0090b5dd  750f                     jne 0x90b5ee
0090b5df  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b5e2  418d490a                 lea ecx, [r9 + 0xa]
0090b5e6  0fabca                   bts edx, ecx
0090b5e9  895720                   mov dword ptr [rdi + 0x20], edx
0090b5ec  eb04                     jmp 0x90b5f2
0090b5ee  44014060                 add dword ptr [rax + 0x60], r8d
0090b5f2  40386864                 cmp byte ptr [rax + 0x64], bpl
0090b5f6  750f                     jne 0x90b607
0090b5f8  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b5fb  418d490b                 lea ecx, [r9 + 0xb]
0090b5ff  0fabca                   bts edx, ecx
0090b602  895720                   mov dword ptr [rdi + 0x20], edx
0090b605  eb04                     jmp 0x90b60b
0090b607  4401406c                 add dword ptr [rax + 0x6c], r8d
0090b60b  40386870                 cmp byte ptr [rax + 0x70], bpl
0090b60f  750f                     jne 0x90b620
0090b611  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b614  418d490c                 lea ecx, [r9 + 0xc]
0090b618  0fabca                   bts edx, ecx
0090b61b  895720                   mov dword ptr [rdi + 0x20], edx
0090b61e  eb04                     jmp 0x90b624
0090b620  44014078                 add dword ptr [rax + 0x78], r8d
0090b624  4038687c                 cmp byte ptr [rax + 0x7c], bpl
0090b628  750f                     jne 0x90b639
0090b62a  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b62d  418d490d                 lea ecx, [r9 + 0xd]
0090b631  0fabca                   bts edx, ecx
0090b634  895720                   mov dword ptr [rdi + 0x20], edx
0090b637  eb07                     jmp 0x90b640
0090b639  44018084000000           add dword ptr [rax + 0x84], r8d
0090b640  4038a888000000           cmp byte ptr [rax + 0x88], bpl
0090b647  750f                     jne 0x90b658
0090b649  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b64c  418d490e                 lea ecx, [r9 + 0xe]
0090b650  0fabca                   bts edx, ecx
0090b653  895720                   mov dword ptr [rdi + 0x20], edx
0090b656  eb07                     jmp 0x90b65f
0090b658  44018090000000           add dword ptr [rax + 0x90], r8d
0090b65f  4038a894000000           cmp byte ptr [rax + 0x94], bpl
0090b666  750f                     jne 0x90b677
0090b668  8b5720                   mov edx, dword ptr [rdi + 0x20]
0090b66b  418d490f                 lea ecx, [r9 + 0xf]
0090b66f  0fabca                   bts edx, ecx
0090b672  895720                   mov dword ptr [rdi + 0x20], edx
0090b675  eb07                     jmp 0x90b67e
0090b677  4401809c000000           add dword ptr [rax + 0x9c], r8d
0090b67e  4983c210                 add r10, 0x10
0090b682  4805c0000000             add rax, 0xc0
0090b688  4983fa20                 cmp r10, 0x20
0090b68c  0f824efeffff             jb 0x90b4e0
0090b692  488b7c2468               mov rdi, qword ptr [rsp + 0x68]
0090b697  498bc5                   mov rax, r13
0090b69a  41896d04                 mov dword ptr [r13 + 4], ebp
0090b69e  41886d00                 mov byte ptr [r13], bpl
0090b6a2  4883c430                 add rsp, 0x30
0090b6a6  415f                     pop r15
0090b6a8  415d                     pop r13
0090b6aa  415c                     pop r12
0090b6ac  5e                       pop rsi
0090b6ad  5d                       pop rbp
0090b6ae  c3                       ret 
