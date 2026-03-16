# Folder Manager 规则格式方案

## 1. 目标

当前旧规则格式如下：

```conf
block|com.tencent.mm|/storage/emulated/0/DCIM/A-TEST|
```

这个格式对程序友好，但对用户不友好，主要问题有：

- 依赖字段位置理解，阅读成本高
- 规则一多就难以维护
- 必须手写完整绝对路径，容易写错
- 无法自然表达后续的白名单能力
- 强迫用户理解 `tree` / `exact` 这类实现细节

因此，本方案的目标是：

- 让普通用户可以直接看懂并手写规则
- 支持黑名单与白名单两种模式
- 不再要求用户理解 `tree` / `exact`
- 保持运行时判定逻辑简单、稳定、高效
- 允许当前阶段进行破坏性改动，不保留旧格式兼容

## 2. 最终选择

采用一套极简规则格式，配置文件固定为：

```text
config/rules.ini
```

核心设计原则：

- 使用分组表示目标应用包名
- 使用 `mode` 表示黑名单或白名单
- 使用 `+` 表示允许访问
- 使用 `-` 表示禁止访问或隐藏
- 使用 `!` 表示写入后自动删除
- 使用 `源路径 -> 目标路径` 表示**静态重定向**（bind mount）
- 使用 `源路径 => 目标路径` 表示**动态重定向**（写入完成后异步搬运）
- 重定向规则可追加 `@types=` 进行文件类型过滤
- 对外语法不暴露 `tree` / `exact`
- 目录和文件的区别由解析阶段与运行时上下文共同推断

## 3. 最终规则写法

推荐用户配置如下：

```ini
# Folder Manager 规则 - 极简写法 2025
#
# 三句话记住全部规则：
#   + 路径              -> 允许这个路径
#   - 路径              -> 禁止/隐藏这个路径
#   ! 路径              -> 写入后自动删除
#   源路径 -> 目标路径   -> 静态重定向（bind mount）
#   源路径 => 目标路径   -> 动态重定向（写入完成后搬运）
#   源路径 -> 目标路径 @types=jpg,png  -> 仅对指定类型重定向
#
# 路径不以 / 开头时，会自动补成 /storage/emulated/0/
# 用户不需要关心 tree / exact，程序会自动推断

[com.tencent.mm]
mode = whitelist
delete_existing = false
delete_dirs = none

+ Pictures/Share
+ Download/Send
+ DCIM/Camera

- DCIM/A-TEST
- Download/private.txt
! Download/Cache

DCIM/Camera -> Android/data/<pkg>/cache/Camera
DCIM/Camera -> Android/data/<pkg>/cache/Camera @types=jpg,png
Movies/Clip -> Android/data/<pkg>/cache/Clip
DCIM/WeiXin => Pictures/WeiXin_Archive

[com.tencent.mobileqq]
mode = whitelist

+ DCIM/Camera
+ Pictures/QQ
- DCIM/敏感
Tencent/QQfile_recv -> Android/data/<pkg>/Tencent/QQfile_recv_hidden
```

## 4. 语法说明

### 4.1 分组

每个分组就是一个目标应用，分组名直接使用包名：

```ini
[com.tencent.mm]
[com.tencent.mobileqq]
```

说明：

- 不使用 `wechat`、`qq` 这类别名作为正式语法
- UI 层可以显示别名，但配置文件层统一使用真实包名
- 这样可以避免别名维护失控

### 4.2 `mode`

每个分组必须显式写出模式：

```ini
mode = whitelist
```

或：

```ini
mode = blacklist
```

说明：

- `mode` 必填，不提供默认值
- 强制显式声明，避免用户误解默认行为

### 4.3 `enabled`

用于临时开关某个包名的整组规则：

```ini
enabled = true
```

或：

```ini
enabled = false
```

说明：

- 未配置时默认 `enabled = true`
- 当 `enabled = false` 时，该包名规则不会被加载

### 4.3 `+ 路径`

表示允许访问该路径。

示例：

```ini
+ Pictures/Share
+ Download/Send
+ Download/demo.jpg
```

### 4.4 `- 路径`

表示禁止访问该路径，并尽量从文件访问、目录枚举、媒体查询中隐藏。

示例：

```ini
- DCIM/A-TEST
- Movies/Nagram
- Download/private.txt
```

### 4.5 `源路径 -> 目标路径`

表示**静态重定向**（bind mount）。源路径与目标路径共享同一份数据：

- App 访问源路径，实际落到目标路径
- 双向透明，零 I/O 开销
- 适合“卸载即焚”的隔离目录

示例：

```ini
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
Movies/Clip -> Android/data/com.tencent.mm/cache/Clip
```

说明：

- `->` 为静态重定向（bind mount）
- 默认保持后缀路径不变
- 同一源路径不建议同时配置 `->` 与 `=>`
- 可选追加 `@types=jpg,png` 仅重定向指定文件类型

### 4.6 `源路径 => 目标路径`

表示**动态重定向**（写入完成后异步搬运）：

- 由后台守护进程监控写入事件（fanotify）
- 文件写入完成后搬运到目标路径
- 适合“强制导出/防后删”的场景

示例：

```ini
DCIM/WeiXin => Pictures/WeiXin_Archive
Tencent/QQfile_recv => Download/QQ
```

说明：

- `=>` 为动态重定向（异步搬运）
- 不参与热路径判定，仅由守护进程处理
- 同一源路径优先 `->`，避免与 `=>` 规则冲突
- 可选追加 `@types=mp4,mov` 仅重定向指定文件类型

### 4.7 `! 路径`

表示**写入完成后自动删除**该路径对应的文件：

- 由守护进程监听写入完成事件（fanotify）
- 命中后执行删除，不改变访问控制结果
- 目录规则会删除目录**下**写入完成的文件（不会主动删除目录）

示例：

```ini
! Download/Cache
! DCIM/Temp
```

说明：

- 仅处理新写入文件，不主动清理历史文件
- 若同时存在 `-` 规则，拦截优先，删除不会触发

### 4.8 删除扩展选项

用于控制删除规则的“历史清理”和“目录删除”策略：

```ini
delete_existing = true
delete_dirs = empty   # none | empty | recursive
```

说明：

- `delete_existing = true`：模块启动或规则重载时，清理已存在的文件
- `delete_dirs = none`：仅删除文件，不删除目录
- `delete_dirs = empty`：删除文件后，顺便清理空目录（向上回溯到规则根目录）
- `delete_dirs = recursive`：允许递归删除规则目录（包含子目录）

### 4.9 重定向类型过滤

重定向规则后可追加 `@types=`，用逗号分隔扩展名（不含点号）：

```ini
DCIM/Camera -> Android/data/<pkg>/cache/Camera @types=jpg,png,heic
Movies/Clip => Download/Clips @types=mp4,mov
```

说明：

- 仅对重定向规则生效
- 按文件扩展名匹配（大小写不敏感）
- 目录规则会用**子文件路径**判断类型

### 4.10 MediaStore 查询过滤开关

用于控制 `MediaStore` 查询过滤是否启用：

```ini
media_query = auto     # auto | true | false
```

说明：

- `auto`：默认策略（白名单或存在 `block` 时启用）。
- `true`：强制启用过滤。
- `false`：强制关闭过滤。

### 4.11 路径通配符（Glob）

规则路径支持 `*` / `?` / `**` 进行**路径匹配**，允许出现在目录段或文件名段：

```ini
- DCIM/*/IMG_*.jpg
- DCIM/**/IMG_*.jpg
DCIM/Camera/**/IMG_*.jpg -> Android/data/<pkg>/cache
```

说明：

- `*` 匹配**单个路径段**，不会跨 `/`
- `?` 匹配**单个字符**，不会跨 `/`
- `**` 作为**独立路径段**时匹配跨目录路径，可以包含 `/`
- `**` 需要连续两个 `*` 才生效（推荐写成 `.../**/...`）
- 重定向时会保留通配符基准目录之后的相对路径

### 4.12 `<pkg>` 占位符

规则中出现 `<pkg>` 时，解析阶段自动替换为当前分组包名。

示例：

```ini
DCIM/Camera -> Android/data/<pkg>/cache/Camera
+ Android/data/<pkg>/
```

## 5. 路径规则

### 5.1 相对路径

如果路径不以 `/` 开头，则自动补全为：

```text
/storage/emulated/0/<路径>
```

例如：

```ini
- DCIM/A-TEST
```

等价于：

```text
- /storage/emulated/0/DCIM/A-TEST
```

### 5.2 绝对路径

如果路径以 `/` 开头，则保持原样。

例如：

```ini
- /data/media/0/DCIM/A-TEST
```

### 5.3 空格与中文路径

规则解析时：

- `+` 或 `-` 行：去掉前缀后，剩余整行视为路径
- 重定向行：按第一个 `->` 切分左右两边

这意味着以下路径应当被正确支持：

```ini
- DCIM/My Album
- DCIM/隐私目录
My Album/Camera -> Android/data/com.tencent.mm/cache/My Album/Camera
```

### 5.4 路径规范化

加载规则时统一规范化：

- `/sdcard/...` -> `/storage/emulated/0/...`
- `/storage/self/primary/...` -> `/storage/emulated/0/...`
- 去除末尾多余 `/`

目标：

- 同一目录只需写一条规则
- 降低路径别名导致的匹配偏差

### 5.5 多用户路径展开（userId）

运行期实际挂载与匹配时，路径应按用户展开：

- `userId = uid / 100000`
- 主用户：`/storage/emulated/0`、`/data/media/0`
- 工作资料/多用户：`/storage/emulated/<userId>`、`/data/media/<userId>`

建议在编译阶段保留**逻辑路径**（统一为 `/storage/emulated/0/...`），
运行期再按 `userId` 生成等价路径列表（至少包含 `/storage/emulated/<userId>` 与 `/data/media/<userId>`）。

## 6. 用户视角下的语义

对用户来说，不需要再思考 `tree` 和 `exact`。

用户只需要按照直觉写路径：

- 写的是文件，就表示这个文件
- 写的是文件夹，就表示这个文件夹

例如：

```ini
- DCIM/A-TEST
- Download/private.txt
```

用户直觉上理解为：

- `DCIM/A-TEST` 是一个目录规则
- `Download/private.txt` 是一个文件规则

这就是我们希望保留的体验。

## 7. 更智能地判断路径是文件还是文件夹

结论：

- 需要在**规则加载阶段尽量判断一次**
- 需要在**运行时结合 syscall 上下文继续修正**
- 不能在**每次 Hook 热路径**里反复做磁盘探测

也就是说，最佳方案不是单一判断，而是：

- 加载期静态探测
- 运行期行为上下文推断
- 内存缓存学习

### 7.1 第一层：加载期静态判断

当模块加载 `rules.ini` 时，对每条规则先做一次静态判断。

推荐顺序如下：

1. **尾部斜杠提示**
   - 如果用户写了 `DCIM/A-TEST/`
   - 则直接视为目录规则
   - 这是一个自然的路径提示，而不是额外语法负担

2. **`lstat()` / `stat()` 检查已存在路径**
   - 如果路径当前已经存在
   - `S_ISDIR(st_mode)` -> 判定为目录
   - `S_ISREG(st_mode)` -> 判定为普通文件

3. **扩展名启发式**
   - 如果路径当前不存在
   - 且明显带有文件扩展名，如 `.txt`、`.jpg`、`.png`、`.mp4`、`.db`、`.json`、`.xml`、`.apk`
   - 则优先推断为文件

4. **默认兜底**
   - 如果既没有尾部 `/`
   - 又不存在
   - 也没有明显扩展名
   - 则默认按目录处理

原因：

- 用户写无扩展名路径时，绝大多数是在表达目录
- 目录规则是更常见的使用方式

### 7.2 第二层：运行期行为上下文推断

仅靠解析阶段仍然不够，因为很多目标路径在规则加载时还不存在。

例如：

- 应用准备创建一个新目录
- 应用准备生成一个新文件
- 用户先配置规则，后续才有内容出现

这时，路径本身只是字符串，系统也不知道它未来是什么。

所以运行时需要结合 syscall 行为上下文继续推断。

#### 明确表示目录的行为

以下行为几乎可以直接判定目标是目录：

- `mkdir()` / `mkdirat()`
- `rmdir()`
- `opendir()`
- `open()` / `openat()` 且带 `O_DIRECTORY`

#### 明确表示文件的行为

以下行为通常可以判定目标更像文件：

- `fopen()` / `fopen64()`
- `open()` / `openat()` 且带 `O_CREAT`，同时没有 `O_DIRECTORY`
- 普通文件读写路径

#### 弱提示行为

以下行为可以辅助判断，但不适合作为唯一依据：

- `stat()` / `lstat()`
- `access()`
- `readlink()`

### 7.3 第三层：路径类型缓存

推荐在内存中维护一个已解析的路径类型缓存，例如：

```text
ResolvedPathKindCache {
  "/storage/emulated/0/DCIM/A-TEST" -> dir
  "/storage/emulated/0/Download/private.txt" -> file
}
```

缓存来源包括：

- 加载期 `lstat()` 结果
- 运行期 `mkdir` / `open(O_DIRECTORY)` / `fopen` 等行为推断结果

这样做的好处：

- 同一路径第一次模糊，后面会越来越准确
- 不需要每次都重新猜测
- 运行时可以持续变聪明

## 8. 程序是否需要判断“文件夹 / 文件”是否存在？

结论：

- 需要在**规则加载阶段尝试判断一次**
- 绝不能在**每次 Hook 热路径**里反复判断

### 8.1 为什么需要判断

虽然用户不需要关心 `tree` / `exact`，但对引擎来说，目录规则和文件规则的执行语义并不完全一样。

例如：

- 文件规则：只匹配这个文件本身
- 目录规则：要匹配目录本身，以及目录下所有子路径

否则会出现问题：

- 应用还能 `stat` 目录本身
- 应用还能 `opendir` 目录本身
- 应用可能看到目录存在，只是目录为空

所以在内部实现上，目录规则必须编译成：

- 匹配目录本身
- 匹配目录内全部后代路径

### 8.2 正确的判断时机

判断应放在**规则加载期**，而不是运行时热路径。

推荐流程：

1. 读取 `rules.ini`
2. 自动补全并规范化路径
3. 对规则目标路径做一次 `lstat/stat`
4. 如果路径存在：
   - 是目录 -> 编译成目录规则
   - 是文件 -> 编译成文件规则
5. 如果路径不存在：
   - 不依赖磁盘结果
   - 改用启发式推断与运行期修正

### 8.3 最重要的原则

这个判断**绝不能放在 Hook 热路径里做**。

也就是说，不应该每次 `open/openat/stat/opendir` 时再去额外 `stat` 一次目标路径。

原因：

- 会增加系统调用开销
- 会拉低拦截性能
- 可能引入递归与副作用
- 不符合当前模块对性能的要求

因此，最终原则是：

- 用户层不暴露复杂匹配概念
- 加载期尽量推断一次
- 运行期利用 syscall 语义继续修正
- 运行时只使用已经编译好的规则对象与缓存结果

## 9. 内部匹配语义

虽然对外不暴露 `tree` / `exact`，但对内仍应保留明确的路径类型概念。

推荐内部三态：

- `auto`
- `file`
- `dir`

建议语义如下：

- `file`：只匹配这个文件本身
- `dir`：匹配目录本身，以及目录内所有后代路径
- `auto`：先查静态推断结果与缓存，再由运行时上下文修正

### 9.1 目录匹配的正确方式

目录规则不能简单裸用 `startsWith(rule_path)`，否则容易误伤兄弟路径。

例如规则为：

```text
/storage/emulated/0/DCIM/A-TEST
```

如果直接做前缀匹配，下面这个路径会被错误命中：

```text
/storage/emulated/0/DCIM/A-TEST_backup
```

因此目录规则必须按以下语义匹配：

- `path == rule_path`
- 或 `path` 以 `rule_path + "/"` 开头

这才是安全的目录匹配方式。

## 10. 黑名单与白名单语义

### 10.1 黑名单模式

`mode = blacklist` 的语义：

- 默认允许访问
- 命中 `-` 时拒绝访问
- 命中重定向规则时执行重定向
- 未命中规则时放行

适合场景：

- 只想屏蔽少量敏感目录
- 不希望影响应用大部分正常访问

### 10.2 白名单模式

`mode = whitelist` 的语义：

- 默认拒绝访问
- 只有命中 `+` 才允许访问
- 命中 `-` 时显式拒绝
- 命中重定向规则时，视为允许并执行重定向
- 未命中任何允许规则时，默认拒绝

适合场景：

- 只允许应用访问极少数目录
- 对外部存储访问做强约束

## 11. 冲突处理规则

建议使用以下优先级：

1. 路径更长的规则优先
2. 如果同一路径上同时存在文件规则和目录规则，则文件规则优先
3. 同样具体程度下，动作优先级为：`-` > `->` > `=>` > `+`

说明：

- 更具体的规则应覆盖更泛的规则
- 显式拒绝应优先于允许
- 静态重定向优先于动态重定向
- 任意重定向优先于普通允许

示例：

```ini
[com.tencent.mm]
mode = whitelist

+ Pictures
- Pictures/Private
Pictures/Camera -> Android/data/com.tencent.mm/cache/Camera
```

判定结果：

- `Pictures/Share/a.jpg` -> 允许
- `Pictures/Private/a.jpg` -> 拒绝
- `Pictures/Camera/a.jpg` -> 重定向

## 12. 当前阶段的边界

第一阶段建议只支持：

- INI 分组
- `mode = whitelist | blacklist`
- `+` / `-` / `->` / `=>`
- 相对路径自动补全
- 路径规范化
- 中文路径与空格路径
- 加载期文件 / 目录推断
- 运行期 syscall 上下文修正
- 内存缓存学习
- `accessible_folders` / `export_folders` 子节解析

第一阶段不建议支持：

- 通配符
- 正则表达式
- 别名语法
- 多层继承
- 复杂作用域
- 运行时频繁磁盘探测
- 旧格式兼容

原因：

- 保持 KISS
- 当前阶段优先快速验证黑白名单语义闭环
- 降低 native 判定复杂度
- 让热路径保持最小开销

## 13. 对内实现建议

虽然对外规则已经极简，但对内仍应解析成结构化规则对象，而不是在热路径继续解析字符串。

建议运行时内部结构类似：

```text
AppPolicy {
  package = com.tencent.mm
  mode = whitelist
  rules = [
    allow dir  /storage/emulated/0/Pictures/Share,
    allow dir  /storage/emulated/0/Download/Send,
    deny  dir  /storage/emulated/0/DCIM/A-TEST,
    deny  file /storage/emulated/0/Download/private.txt,
    redirect_static dir /storage/emulated/0/DCIM/Camera -> /storage/emulated/0/Android/data/com.tencent.mm/cache/Camera
    redirect_dynamic dir /storage/emulated/0/DCIM/WeiXin => /storage/emulated/0/Pictures/WeiXin_Archive
  ]
  accessible_rules = [...]
  export_rules = [...]
}
```

说明：

- 配置只在注入初始化时解析一次
- 文件访问热路径只进行结构化判定
- 对外隐藏复杂度，对内保留准确语义

## 14. 最终推荐结论

当前阶段最终推荐使用下面这套规则格式：

```ini
[com.tencent.mm]
mode = whitelist

+ Pictures/Share
+ Download/Send
+ DCIM/Camera

- DCIM/A-TEST
- Download/private.txt

DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
Movies/Clip -> Android/data/com.tencent.mm/cache/Clip
```

采用这套格式的原因：

- 用户一眼能看懂
- 手写成本极低
- 不再重复包名
- 相对路径更符合真实使用习惯
- 不再要求用户理解 `tree` / `exact`
- 黑名单与白名单都可以自然表达
- 同时仍然适合被 native 层解析成高效规则结构
- 通过“加载期静态探测 + 运行期上下文推断 + 内存缓存”获得更智能的路径类型判断

## 15. 实现注意事项与边缘情况

下面这些点不影响用户规则语法，但在未来使用 C/C++ 或 Rust 落地时，建议直接纳入实现设计。

### 15.1 路径类型缓存的线程安全

如果模块在应用进程内对文件访问做 Hook，那么同一个进程里通常会有多个线程并发访问文件系统。

因此，路径类型缓存（例如 `ResolvedPathKindCache`）必须考虑线程安全：

- C++ 可使用 `std::shared_mutex`
- Rust 可使用 `RwLock`
- 读多写少的场景下，优先使用读写锁而不是普通互斥锁

建议：

- 读取缓存时使用共享读锁
- 首次推断或更新缓存时使用独占写锁
- 第一阶段只要求“进程内一致”，不必追求跨进程共享

### 15.2 路径类型缓存的失效问题

缓存不是绝对真理，极小概率下会发生路径类型变化。

例如：

1. 原来路径是一个文件
2. 后来该文件被删除
3. 又在相同路径上新建了一个目录

如果缓存里仍然记着它是 `file`，那么后续匹配会出现偏差。

第一阶段建议：

- 先不做复杂失效机制
- 默认缓存只在当前进程生命周期内有效
- 进程重启后重新学习

后续如需增强，可考虑在拦截这些操作时同步清理对应缓存：

- `unlink`
- `unlinkat`
- `rmdir`
- `rename`
- `renameat`
- `mkdir`
- `mkdirat`

### 15.3 软链接（Symlink）必须使用 `lstat()`

在加载阶段做静态探测时，应优先使用 `lstat()`，而不是 `stat()`。

原因：

- `stat()` 会跟随软链接，看到的是目标对象
- `lstat()` 看到的是当前路径节点本身

对安全拦截来说，`lstat()` 更稳妥，因为：

- 流氓软件可能利用软链接绕过路径限制
- Android 文件系统中也存在符号链接与路径别名
- 如果用 `stat()`，路径语义可能被“穿透”后发生错位

建议：

- 规则加载期静态探测统一优先用 `lstat()`
- 只有在明确需要解析目标节点时，才额外考虑 `stat()`

### 15.4 重定向只执行一次，避免链式递归

重定向规则在逻辑上会涉及一个关键问题：

- 路径 A 被重定向到路径 B 后
- 是否还要对路径 B 再跑一轮规则匹配？

第一阶段建议采用最简单且最稳妥的策略：

- 重定向只执行一次
- 一旦决定把 A 重定向到 B，就直接放行对 B 的访问
- 不再继续对 B 套规则

原因：

- 可以避免形成重定向死循环
- 可以避免 A -> B -> C 这种链式复杂度
- 符合 KISS
- 更容易定位问题和打印日志

典型风险场景：

- A -> B
- B -> A

如果允许二次匹配，会非常容易造成无限递归或逻辑抖动。

建议：

- 在内部结构中增加一个“本次访问已重定向”的状态位
- 一旦命中重定向，后续调用链只使用目标路径，不再重新套规则

### 15.5 缓存与重定向不要相互污染

路径类型缓存建议基于“规范化后的真实访问路径”存储，但要注意区分：

- 原始请求路径
- 重定向目标路径

不要把“源路径的推断结果”直接污染“目标路径的缓存”，反之亦然。

推荐做法：

- 源路径和目标路径分别缓存
- 日志里同时记录 `from` 与 `to`
- 重定向命中后，只对目标路径执行真实访问，不自动继承源路径类型

### 15.6 第一阶段的建议实现边界

为了保持实现简单，当前阶段建议：

- 缓存只在单进程内生效
- 缓存不做复杂失效广播
- 规则重定向只执行一次
- `lstat()` 作为静态探测默认入口
- 不在 Hook 热路径里引入额外磁盘探测

这套边界足够覆盖绝大多数真实场景，同时不会明显增加系统复杂度。

## 16. 扩展能力：`accessible_folders` / `export_folders`

当前这套 `+` / `-` / `->` / `=>` 基础规则，已经足够表达单应用视角下的：

- 允许访问
- 禁止访问
- 路径重定向

但它还不足以优雅表达两类真实需求：

### 16.1 跨应用协作目录

典型场景：

- 输入法把表情或图片临时放到自己的目录里，微信/QQ/TIM 需要读取
- 文件管理器需要读取微信或 QQ 的共享内容目录
- 某个插件或配套应用，需要访问宿主应用约定的共享目录

这种需求本质上不是简单的“当前应用允许访问哪个目录”，而是：

- **A 应用生成或拥有某个目录**
- **B 应用需要被允许访问这个目录**

这正是 `accessible_folders` 要表达的能力。

### 16.2 导出到标准目录

典型场景：

- QQ 把接收的文件保存到 `tencent/QQfile_recv`
- TIM 把图片保存到 `tencent/Tim_Images`
- 某些应用把“用户真正关心的文件”保存到非标准目录

此时用户真正想要的不是单纯“允许访问”，而是：

- 把这些有价值的文件导出到标准目录
- 可选触发媒体库刷新
- 可选登记到下载列表

这正是 `export_folders` 要表达的能力。

### 16.3 扩展原则

扩展建议严格遵守以下原则：

- **不破坏当前基础规则语法**
- **继续使用 INI 分组，不引入新的大括号或脚本语言**
- **一条扩展规则对应一个独立子节，避免数组和复杂嵌套**
- **路径继续沿用当前的相对路径补全和规范化规则**
- **`accessible_folders` 进入匹配体系**
- **`export_folders` 作为声明式元数据，优先交给 GUI / 辅助服务处理**

也就是说：

- `accessible_folders` 更接近“访问控制规则”
- `export_folders` 更接近“导出任务规则”

## 17. `accessible_folders` 规则草案

### 17.1 语义定义

`accessible_folders` 用来表达：

- 允许 `to` 应用访问 `from` 应用相关的某个目录

推荐将其理解为：

- **跨应用的目录级白名单例外规则**

它主要解决以下问题：

- “可以看到，但发送不了”
- “插件能启动，但读不到宿主共享文件”
- “文件管理器无法读取目标应用内容”

### 17.2 推荐语法

推荐在原有应用分组之外，新增**子节**：

```ini
[com.tencent.mm.accessible.qqinput_temp]
from = com.tencent.qqpinyin
path = tencent/QQInput/Ext/Temp
description = 修复 QQ 拼音向微信发送图片

[com.tencent.mm.accessible.tencent_filemanager]
to = com.tencent.FileManager
path = tencent
description = 允许腾讯文件管理器读取微信内容
```

说明：

- 节名结构：`[<锚点包名>.accessible.<规则名>]`
- `规则名` 仅作为人类可读 ID，不参与运行时匹配
- 建议只使用：小写字母、数字、下划线

### 17.3 字段说明

#### `from`

表示目录的来源应用。

例如：

```ini
from = com.tencent.qqpinyin
```

含义：

- 该目录由 `com.tencent.qqpinyin` 产生、拥有或约定使用

#### `to`

表示需要被允许访问该目录的目标应用。

例如：

```ini
to = com.tencent.FileManager
```

#### `path`

表示被开放的目录路径。

例如：

```ini
path = tencent/QQInput/Ext/Temp
```

路径规则与基础规则完全一致：

- 不以 `/` 开头时，自动补成 `/storage/emulated/0/...`
- 以 `/` 开头时，按绝对路径处理
- 加载时统一做规范化

#### `description`

表示规则说明，仅用于：

- GUI 展示
- 日志说明
- 配置维护

不参与实际匹配。

### 17.4 默认值约定

为了减少书写负担，建议采用以下默认值：

- 在 `[pkg.accessible.xxx]` 子节内：
  - 如果写了 `from`，但没写 `to`，则默认 `to = pkg`
  - 如果写了 `to`，但没写 `from`，则默认 `from = pkg`

例如：

```ini
[com.tencent.mm.accessible.qqinput_temp]
from = com.tencent.qqpinyin
path = tencent/QQInput/Ext/Temp
```

等价于：

```ini
from = com.tencent.qqpinyin
to = com.tencent.mm
path = tencent/QQInput/Ext/Temp
```

再例如：

```ini
[com.tencent.mm.accessible.tencent_filemanager]
to = com.tencent.FileManager
path = tencent
```

等价于：

```ini
from = com.tencent.mm
to = com.tencent.FileManager
path = tencent
```

### 17.5 约束建议

为了保持实现简单，第一阶段建议：

- `accessible_folders` 只支持**目录路径**
- 一个子节只写一个 `path`
- 多个目录就写多个子节
- 不支持通配符
- 不支持正则

这样做的好处是：

- 规则更清晰
- 解析器更简单
- 更适合未来 GUI 一条一条展示和开关

### 17.6 与运行时匹配的关系

`accessible_folders` 不应该只是“展示层规则”，而应进入统一判定模型。

推荐内部语义为：

- 当当前进程包名等于 `to`
- 且访问路径命中 `path`
- 则将其视为一个额外的 allow 例外规则

尤其要注意：

- **文件访问判定**要应用这条规则
- **MediaStore / 媒体查询可见性判定**也要应用这条规则

只有这样，才能尽量避免出现：

- 列表里能看到
- 实际发送或打开时失败

## 18. `export_folders` 规则草案

### 18.1 语义定义

`export_folders` 用来表达：

- 某个应用把“用户真正关心的文件”保存到了非标准目录
- 系统需要把它导出到更标准、更容易被系统和其他应用识别的位置

它不是普通的访问控制规则，而更接近：

- **导出任务声明**

### 18.2 推荐语法

同样使用子节表示：

```ini
[com.tencent.mobileqq.export.saved_files]
source = tencent/QQfile_recv
target = Download/QQ
title = 保存的文件
media_scan = true
add_to_downloads = true
allow_child = false
description = 将 QQ 接收的文件导出到标准下载目录

[com.tencent.mobileqq.export.saved_images]
source = tencent/QQ_Images
target = Pictures/QQ
title = 保存的图片
media_scan = true
add_to_downloads = false
allow_child = false
description = 将 QQ 保存的图片导出到系统图片目录
```

节名结构：

```ini
[<包名>.export.<规则名>]
```

### 18.3 字段说明

#### `source`

导出的来源路径。

例如：

```ini
source = tencent/QQfile_recv
```

#### `target`

导出的目标路径。

例如：

```ini
target = Download/QQ
```

#### `title`

用于 GUI 展示的人类可读标题。

例如：

```ini
title = 保存的图片
```

#### `media_scan`

表示导出完成后，是否请求系统刷新媒体库。

例如：

```ini
media_scan = true
```

适用场景：

- 图片
- 视频
- 音频

#### `add_to_downloads`

表示导出完成后，是否将文件登记到下载列表。

例如：

```ini
add_to_downloads = true
```

适用场景：

- 文档
- 安装包
- 用户主动接收的文件

实现说明（当前版本）：

- 由动态守护进程调用 `/system/bin/content insert`
- 写入 `content://downloads/my_downloads`（DownloadProvider）
- 采用 `DESTINATION_NON_DOWNLOADMANAGER_DOWNLOAD` + `STATUS_SUCCESS` 语义
- `media_scan = true` 时写入 `media_scanned = 0`（可被扫描）
- 若 `content` 不存在或权限不足，只记录日志，不影响导出流程

#### `allow_child`

表示是否允许把子目录内容也一起纳入导出范围。

例如：

```ini
allow_child = false
```

第一阶段建议：

- 默认为 `false`
- 仅在确有需求时开启

#### `description`

仅用于 GUI / 日志展示，不参与实际导出逻辑判断。

### 18.4 行为边界建议

为了避免把 native 热路径做得过重，建议明确边界：

- `export_folders` **不是**每次 `open` / `stat` 时都要执行的规则
- `export_folders` 应由 GUI、伴生服务或手动导出动作触发
- native 核心只负责：
  - 解析规则
  - 提供结构化规则数据
  - 必要时提供路径映射辅助

也就是说：

- `accessible_folders` 属于**运行时访问控制能力**
- `export_folders` 属于**导出工作流能力**

这两者不要混在同一个热路径判定函数里。

### 18.5 第一阶段建议的导出语义

当前阶段建议把“导出”定义为：

- **复制**到目标目录，而不是移动

原因：

- 更安全
- 更符合用户预期
- 不会破坏原应用内部引用关系
- 更方便失败后重试

未来如果确有需要，再增加：

- `mode = copy`
- `mode = move`
- `mode = mirror`

当前阶段先不引入，遵守 YAGNI。

## 19. 完整配置示例

下面给出一个把基础规则、`accessible_folders`、`export_folders` 放在一起的完整示例：

```ini
[com.tencent.mm]
mode = whitelist

+ DCIM/Camera
+ Pictures/Share
- DCIM/A-TEST
DCIM/Camera -> Android/data/com.tencent.mm/cache/Camera
DCIM/WeiXin => Pictures/WeiXin_Archive

[com.tencent.mm.accessible.qqinput_temp]
from = com.tencent.qqpinyin
path = tencent/QQInput/Ext/Temp
description = 修复 QQ 拼音向微信发送图片

[com.tencent.mm.accessible.tencent_filemanager]
to = com.tencent.FileManager
path = tencent
description = 允许腾讯文件管理器读取微信内容

[com.tencent.mobileqq]
mode = whitelist

+ DCIM/Camera
+ Pictures/QQ
- DCIM/敏感

[com.tencent.mobileqq.export.saved_files]
source = tencent/QQfile_recv
target = Download/QQ
title = 保存的文件
media_scan = true
add_to_downloads = true
allow_child = false
description = 将 QQ 接收的文件导出到标准下载目录

[com.tencent.mobileqq.export.saved_images]
source = tencent/QQ_Images
target = Pictures/QQ
title = 保存的图片
media_scan = true
add_to_downloads = false
allow_child = false
description = 将 QQ 保存的图片导出到系统图片目录
```

## 20. 内部数据结构建议

为了与当前基础规则统一，建议在内部编译成以下结构：

```text
AppPolicy {
  package_name
  mode
  rules[]
  accessible_rules[]
  export_rules[]
}

AccessibleFolderRule {
  id
  from_package
  to_package
  path
  description
}

ExportFolderRule {
  id
  package_name
  source
  target
  title
  media_scan
  add_to_downloads
  allow_child
  description
}
```

建议的职责边界：

- `rules[]`：native 热路径直接匹配
- `accessible_rules[]`：native 匹配层 + 媒体可见性层共同使用
- `export_rules[]`：GUI / 服务层消费

## 21. 最终建议

当前阶段建议正式采用以下扩展路线：

### 21.1 基础规则保持不变

继续保留：

- `[pkg]`
- `mode = whitelist | blacklist`
- `+ 路径`
- `- 路径`
- `源 -> 目标`
- `源 => 目标`
- `<pkg>` 占位符

### 21.2 新增两类子节

新增：

- `[pkg.accessible.rule_id]`
- `[pkg.export.rule_id]`

### 21.3 优先实现顺序

建议优先级如下：

1. 先稳定解析与编译流程（基础规则 + 子节）
2. 再补齐 `accessible_folders`
3. 最后落地 `export_folders` 与 GUI 编辑器

原因：

- `accessible_folders` 直接关系到“能不能发送、能不能分享、能不能读取”
- `export_folders` 更偏向用户体验增强
- GUI 适合建立在稳定规则模型之上，而不是反过来驱动底层规则设计

### 21.4 对当前项目的直接价值

如果后续要解决：

- 微信/QQ/TIM 选择器里能看到，但发不出去
- 输入法表情、临时图片无法发送
- 文件管理器无法读取目标应用内容

那么 `accessible_folders` 应该成为核心能力。

如果后续要解决：

- 应用把图片、文件保存到非标准目录
- 用户希望一键导出到 `Pictures/`、`Download/` 等标准位置

那么 `export_folders` 应该由 GUI / 辅助服务承接。

## 22. C++ 解析结构体与编译流程（融合版）

### 22.1 解析期结构体（AST）

```cpp
enum class Mode { Blacklist, Whitelist };
enum class RuleAction { Allow, Deny, RedirectStatic, RedirectDynamic };
enum class PathKind { Auto, File, Dir };

struct RuleAst {
  RuleAction action;
  std::string raw_path;
  std::string raw_target; // redirect 时有效
  int line;
};

struct AppSectionAst {
  std::string package;
  Mode mode;
  std::vector<RuleAst> rules;
};

struct AccessibleAst {
  std::string id;
  std::string from_pkg;
  std::string to_pkg;
  std::string path;
  std::string description;
};

struct ExportAst {
  std::string id;
  std::string package;
  std::string source;
  std::string target;
  std::string title;
  bool media_scan;
  bool add_to_downloads;
  bool allow_child;
  std::string description;
};

struct RulesAst {
  std::vector<AppSectionAst> apps;
  std::vector<AccessibleAst> accessible;
  std::vector<ExportAst> exports;
};
```

### 22.2 编译期结构体（运行时）

```cpp
struct CompiledRule {
  RuleAction action;
  PathKind kind;
  std::string path;       // 规范化后的绝对路径
  std::string target;     // 重定向目标（可空）
  int priority;           // 路径深度（'/' 数量）
};

struct AccessibleRule {
  std::string from_pkg;
  std::string to_pkg;
  std::string path;       // 规范化后的绝对路径
};

struct ExportRule {
  std::string package;
  std::string source;
  std::string target;
  std::string title;
  bool media_scan;
  bool add_to_downloads;
  bool allow_child;
  std::string description;
};

struct AppPolicy {
  std::string package;
  Mode mode;
  std::vector<CompiledRule> rules;          // allow/deny/redirect（含 =>）
  std::vector<AccessibleRule> accessible;   // 仅对 to_pkg 生效
  std::vector<ExportRule> exports;          // GUI/服务层消费
  PathTrie trie;                            // longest-prefix 匹配
};
```

### 22.3 编译流程（关键步骤）

1. **读取与分段**：UTF-8 + LF 读取 `rules.ini`，忽略空行与 `#` 注释。  
2. **分组解析**：识别 `[pkg]`、`[pkg.accessible.id]`、`[pkg.export.id]` 三类节。  
3. **规则行解析**：识别 `+` / `-` / `->` / `=>`，提取 `raw_path` 与 `raw_target`。  
4. **占位符替换**：将 `<pkg>` 替换为当前分组包名。  
5. **相对路径补全**：非 `/` 开头路径补全为 `/storage/emulated/0/<path>`。  
6. **别名规范化**：`/sdcard`、`/storage/self/primary` 统一替换为 `/storage/emulated/0`。  
7. **字面量规范化**：执行 `lexicalNormalize()` 消灭 `.` / `..`。  
8. **路径类型推断**：末尾 `/` → Dir；`lstat()` 成功则尊重；无结果时按扩展名启发式；默认 Dir。  
9. **优先级计算**：按路径深度（`/` 数量）生成 `priority`，同深度按动作权重排序。  
10. **构建 Trie**：将 `rules` 按路径插入 Trie，支持最长前缀匹配。  
11. **动态规则分离**：`RedirectDynamic` 规则单独输出给守护进程（fanotify）。  
12. **序列化输出**：`AppPolicy` 序列化为 POD + 字符串池，写入 memfd；必要时 fallback 到 hash 文件。

### 22.4 运行期要点（热路径约束）

- **禁止热路径 I/O**：匹配阶段只做字符串与 Trie 判断。  
- **多用户路径展开**：运行期按 `userId = uid / 100000` 生成 `/storage/emulated/<userId>` 与 `/data/media/<userId>` 组合路径。  
- **缓存读写锁**：`ResolvedPathKindCache` 使用 `std::shared_mutex` 读多写少。  
- **重定向只执行一次**：命中 `->` / `=>` 后不再递归匹配目标路径。
