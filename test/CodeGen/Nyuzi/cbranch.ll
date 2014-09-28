; RUN: llc -mtriple nyuzi-elf %s -o - | FileCheck %s

target triple = "nyuzi"

; Do a scalar comparison and branch on the result.
define i32 @max(i32 %a, i32 %b) #0 {
entry:
  %cmp = icmp sgt i32 %a, %b
  ; CHECK: cmpgt_i s[[CHECKVAL:[0-9]+]], s{{[0-9]+}}, s{{[0-9]+}}

  br i1 %cmp, label %if.then, label %if.else
  ; CHECK: btrue s[[CHECKVAL]], [[FALSELABEL:[\.A-Z0-9_]+]]

if.then:
  br label %return

if.else:  
  ; CHECK: [[FALSELABEL]]:
  br label %return

return:
  %0 = phi i32 [ %a, %if.then ], [ %b, %if.else ]
  ret i32 %0
}


