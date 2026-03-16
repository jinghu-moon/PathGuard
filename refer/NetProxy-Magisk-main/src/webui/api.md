# KernelSU 模块 WebUI 库

## 安装

```bash
yarn add kernelsu
```

## API 接口

### `exec`

启动一个 root shell 并在其中运行命令，返回一个 Promise，完成后解析为标准输出 (stdout) 和标准错误 (stderr)。

- `command` \<string\>: 要运行的命令，包含空格分隔的参数。
- `options` \<Object\>:
    - `cwd`: 子进程的当前工作目录。
    - `env`: 环境变量键值对。

```javascript
import { exec } from 'kernelsu';

const { errno, stdout, stderr } = await exec('ls -l', { cwd: '/tmp' });
if (errno === 0) {
    // 成功
    console.log(stdout);
}
```

---

### `spawn`

在 root shell 中使用给定命令启动新进程，参数在 args 中指定。如果省略，args 默认为空数组。

返回一个 `ChildProcess` 实例。`ChildProcess` 的实例代表生成的子进程。

- `command` \<string\>: 要运行的命令。
- `args` \<string[]\>: 字符串参数列表。
- `options` \<Object\>:
    - `cwd` \<string\>: 子进程的当前工作目录。
    - `env` \<Object\>: 环境变量键值对。

运行 `ls -lh /data` 的示例，捕获 stdout、stderr 和退出代码：

```javascript
import { spawn } from 'kernelsu';

const ls = spawn('ls', ['-lh', '/data']);

ls.stdout.on('data', (data) => {
  console.log(`stdout: ${data}`);
});

ls.stderr.on('data', (data) => {
  console.log(`stderr: ${data}`);
});

ls.on('exit', (code) => {
  console.log(`子进程退出，退出码: ${code}`);
});
```

#### `ChildProcess`

**事件 'exit'**

- `code` \<number\>:如果子进程自行退出，则为退出代码。

当子进程结束时触发 'exit' 事件。如果进程退出，code 包含最终退出代码；否则为 null。

**事件 'error'**

- `err` \<Error\>: 错误对象。

每当发生以下情况时触发 'error' 事件：
1. 无法启动进程。
2. 无法杀死进程。

**`stdout`**

代表子进程标准输出 (stdout) 的可读流 (Readable Stream)。

```javascript
const subprocess = spawn('ls');

subprocess.stdout.on('data', (data) => {
  console.log(`收到数据块: ${data}`);
});
```

**`stderr`**

代表子进程标准错误 (stderr) 的可读流 (Readable Stream)。

---

### `fullScreen`

请求 WebView 进入/退出全屏模式。

```javascript
import { fullScreen } from 'kernelsu';
fullScreen(true);
```

---

### `enableInsets`

请求 WebView 将内边距设置为 0 或系统栏内边距 (Insets)。

> 提示：此功能默认禁用，但如果您从 internal/insets.css 请求资源，它将自动启用。

要获取内边距值并自动启用此功能，您可以：
1. 在 CSS 中添加 `@import "https://mui.kernelsu.org/internal/insets.css";`
2. 或者在 HTML 中添加 `<link rel="stylesheet" type="text/css" href="/internal/insets.css" />`

```javascript
import { enableInsets } from 'kernelsu';
enableInsets(true);
```

---

### `toast`

显示 Toast 提示消息。

```javascript
import { toast } from 'kernelsu';
toast('Hello, world!');
```

---

### `moduleInfo`

获取模块信息。

```javascript
import { moduleInfo } from 'kernelsu';
// 在控制台打印 moduleId
console.log(moduleInfo());
```

---

### `listPackages`

列出已安装的包。

返回包名数组。

- `type` \<string\>: 要列出的包类型："user" (用户应用), "system" (系统应用), 或 "all" (所有应用)。

```javascript
import { listPackages } from 'kernelsu';
// 列出用户应用
const packages = listPackages("user");
```

> 提示：当 `listPackages` API 可用时，您可以使用 `ksu://icon/{packageName}` 获取应用图标。

```javascript
img.src = "ksu://icon/" + packageName;
```

---

### `getPackagesInfo`

获取应用列表的详细信息。

返回 `PackagesInfo` 对象数组。

- `packages` \<string[]\>: 包名列表。

```javascript
import { getPackagesInfo } from 'kernelsu';
const packages = getPackagesInfo(['com.android.settings', 'com.android.shell']);
```

#### `PackagesInfo`

包含以下字段的对象：

- `packageName` \<string\>: 应用包名。
- `versionName` \<string\>: 应用版本名。
- `versionCode` \<number\>: 应用版本号。
- `appLabel` \<string\>: 应用显示名称。
- `isSystem` \<boolean\>: 是否为系统应用。
- `uid` \<number\>: 应用 UID。