# 第 3 章：`tinyttg` —— TTGIR 是带线程映射的 TTIR

第 2 章结束时，`vecadd.ttir` 已经是一个可验证的 TTIR 模块。但它只描述
“一个 block 里发生了什么”，没有回答“哪个线程持有哪个元素”。这一章补上
TTGIR：

```text
tinytt  : tensor<1024xf32>                      （没有线程映射）
tinyttg : tensor<1024xf32, #tinyttg.blocked<...>>（每个元素有主人）
```

两者是**两个 dialect**，不是一个 dialect 的两种打印方式：`tinyttg` 有
自己的 verifier，缺布局、布局铺不满张量都会在 parse 阶段失败。

## 1. 为什么 12 个 op 只定义一次

TTGIR 的 op 语义与 TTIR 相同，差别只有两点：

1. dialect 前缀不同（`tinytt.` vs `tinyttg.`）；
2. 每个 tensor 必须带布局。

如果复制 12 个 op，第 4 章改一个 verifier 就要改两处。因此本章把 op 定义
抽到 `include/tiny/Dialect/TinyCommon/TinyCommonOps.td`，按 dialect 参数化：

```tablegen
class TinyCommon_GetProgramId<Dialect d, string m, list<Trait> traits = []>
    : Op<d, m, !listconcat([Pure], traits)> { ... }
```

`tinytt` 与 `tinyttg` 各自只写十二行：

```tablegen
def TinyTTG_GetProgramIdOp
    : TinyCommon_GetProgramId<TinyTTG_Dialect, "get_program_id", [TinyTTG_Layouts]>;
```

verifier 同理：语义检查在 `TinyCommon/OpVerifiers.h`，两个 dialect 的
`XxxOp::verify()` 都只是转调。

`!tinytt.ptr` **不复制**。Triton 的 TTGIR 仍然用 `tt.ptr`，本章也一样：
布局只挂在 tensor 上，指针类型继续由 `tinytt` 拥有。

## 2. 布局属性

```mlir
#blocked = #tinyttg.blocked<sizePerThread = [1], threadsPerWarp = [8],
                            warpsPerCTA = [4], order = [0]>
#smem    = #tinyttg.shared<offset = 0, order = [1, 0]>
```

> 属性前缀是 `#tinyttg.`，不是 `#tiny.`：MLIR 的别名前缀就是 dialect 名。
> 第 1 章把 dialect 从 `tiny.tt` 改名为 `tinytt` 后，布局前缀也随之为
> `tinyttg`。

### 2.1 blocked：tile 与重复

一个 tile 在第 `d` 维覆盖

```text
sizePerThread[d] * threadsPerWarp[d] * warpsPerCTA[d]
```

个元素。GPUMin 的一个 block 是 `8 lane/warp × 4 warp = 32` 线程，所以
1D 默认布局覆盖 32 个元素。`tensor<1024xf32>` 比 tile 大：它按 `order`
重复 tile 32 次。

因此 verifier 的规则是**整除**，不是相等：

```text
shape[d] % (sizePerThread[d] * threadsPerWarp[d] * warpsPerCTA[d]) == 0
```

这条规则是本章最容易写错的地方。若要求“布局元素数 == shape 乘积”，
1024 元素就必须有 1024 个线程，与 GPUMin 的 32 线程模型冲突。

### 2.2 shared

`shared` 只有 `offset` 与 `order`，第 6 章的 reduce 与第 7 章的 trans 才会用到。
它现在就注册，是为了让 `tensor<..., #tinyttg.shared<...>>` 在 TTGIR 里
从第 3 章起就是合法类型。

## 3. 五个 TTG-only op

| op | 语法 | 作用 |
| --- | --- | --- |
| `convert_layout` | `%r = tinyttg.convert_layout %v : tensor<...L1> -> tensor<...L2>` | 换分布 |
| `local_alloc` | `%p = tinyttg.local_alloc %v : tensor<...> -> !tinytt.ptr<f32, 2>` | 在 shared 里开 buffer |
| `local_store` | `tinyttg.local_store %v, %p : tensor<...>, !tinytt.ptr<f32, 2>` | 写 shared |
| `local_load` | `%v = tinyttg.local_load %p : !tinytt.ptr<f32, 2> -> tensor<...>` | 读 shared |
| `barrier` | `tinyttg.barrier` | block 内同步 |

它们的 verifier 都检查地址空间：shared 指针必须是 `!tinytt.ptr<f32, 2>`，
全局指针（AS=1）传给 `local_*` 会被拒绝。

## 4. verifier 是本章的交付物

布局检查做成一个 trait，挂在所有带 tensor 的 op 上：

```tablegen
def TinyTTG_Layouts : NativeOpTrait<"tiny::RequireLayouts">;
```

> `NativeOpTrait` 的名字会被拼到 `::mlir::OpTrait::` 后面，所以 C++ 侧的
> trait 定义在 `namespace mlir::OpTrait::tiny` 里。这是 MLIR 生成器的约定，
> 不是本章的发明。

trait 做三件事：

1. 每个 tensor operand/result 必须有 encoding；
2. blocked 的四个数组 rank 必须等于 tensor rank；
3. 每一维必须被 tile 整除。

两条负例分别锁住 1 和 3：

```text
tests/negative/ttg-missing-layout.ttir
  error: 'tinyttg.make_range' op result must carry a #tinyttg layout

tests/negative/ttg-bad-tile.ttir
  error: 'tinyttg.splat' op result: dimension 0 of size 100 is not divisible
         by layout tile 32
```

## 5. golden

`tests/ttg/vecadd.ttgir` 是第 2 章 TTIR 的逐 op 翻译：op 名换成
`tinyttg.`，每个 tensor 补上 `#blocked`，module 上补
`tinyttg.num_warps = 4`。1024 元素 = 32 次 tile 重复，正好验证 2.1 的
整除规则。

```sh
tiny-opt --verify-each tiny-triton/tests/ttg/vecadd.ttgir
```

第一次输出与第二次输出逐字节相同，说明 parse/print 是稳定的。

## 6. CMake 与驱动

- `include/tiny/Dialect/TinyTTG/CMakeLists.txt`：
  `add_mlir_dialect(TinyTTGOps tinyttg)` 生成 op/dialect；
  `-gen-attrdef-decls/defs` 单独生成两个布局属性。
- 属性 storage 只在一个 TU 可见，所以注册走成员函数
  `TinyTTGDialect::registerLayouts()`，与第 1 章的 `registerTypes()` 同构。
- `tiny-opt` 同时注册 `tiny::tt::TinyTTDialect` 与
  `tiny::ttg::TinyTTGDialect`，`--help` 里能看到两个名字。

## 7. 门禁

```sh
tiny-triton/scripts/check-t3.sh /home/ubuntu/llvm/builds/pr-ctpop/bin
```

五项：

1. `tiny-opt --help` 列出 `tinyttg`；
2. `vecadd.ttgir` parse/print 两次结果一致；
3. 缺布局的 tensor 被拒绝，诊断含 `must carry a #tinyttg layout`；
4. 铺不满的布局被拒绝，诊断含 `not divisible by layout tile`；
5. `check-t1.sh` 5/5、`check-t2.sh` 4/4 不回归。

## 8. 退出条件

- `ninja -C <build> tiny-opt tiny-triton` 成功；
- `check-t3.sh` 5/5；`check-t1.sh` 5/5；`check-t2.sh` 4/4；
- 旧管线 `gpumin-verify-all.sh` 8/8、`triton-verify-all.sh` 0 failed；
- `git tag tiny-t03` 指向本章 commit。
