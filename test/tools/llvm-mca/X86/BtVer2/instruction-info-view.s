# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -resource-pressure=false -instruction-info=true < %s | FileCheck %s --check-prefix=ENABLED
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -resource-pressure=false -instruction-info=false < %s | FileCheck %s -check-prefix=DISABLED
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -resource-pressure=false -instruction-info < %s | FileCheck %s -check-prefix=ENABLED
# RUN: llvm-mca -mtriple=x86_64-unknown-unknown -mcpu=btver2 -resource-pressure=false < %s | FileCheck %s -check-prefix=ENABLED

vmulps   %xmm0, %xmm1, %xmm2
vhaddps  %xmm2, %xmm2, %xmm3
vhaddps  %xmm3, %xmm3, %xmm4

# DISABLED-NOT: Instruction Info:

# ENABLED:      Instruction Info:
# ENABLED-NEXT: [1]: #uOps
# ENABLED-NEXT: [2]: Latency
# ENABLED-NEXT: [3]: RThroughput
# ENABLED-NEXT: [4]: MayLoad
# ENABLED-NEXT: [5]: MayStore
# ENABLED-NEXT: [6]: HasSideEffects

# ENABLED:      [1]    [2]    [3]    [4]    [5]    [6]	Instructions:
# ENABLED-NEXT:  1      2     1.00                    	vmulps	%xmm0, %xmm1, %xmm2
# ENABLED-NEXT:  1      3     1.00                    	vhaddps	%xmm2, %xmm2, %xmm3
# ENABLED-NEXT:  1      3     1.00                    	vhaddps	%xmm3, %xmm3, %xmm4
