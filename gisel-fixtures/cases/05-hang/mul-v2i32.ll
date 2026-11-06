; #236649 的最小复现。GlobalISel -O0 下不返回；SelectionDAG 同输入返回 0。
; 这里给 lit 一个 20 秒的短超时；手工复核用 check.sh 的默认 60 秒。
;
; RUN: bash %S/check.sh %llc 20
;
define void @f(ptr %p, ptr %q) {
  %a = load <2 x i32>, ptr %p
  %b = load <2 x i32>, ptr %q
  %r = mul <2 x i32> %a, %b
  store <2 x i32> %r, ptr %p
  ret void
}
