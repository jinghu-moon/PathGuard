# Provider 观测说明

该版本默认开启 `IContentProvider` 观测日志，但不改写 `MediaStore / DownloadsProvider` 请求。

## 目标

用于定位：
- 微信发送图片时，是否经过 `MediaStore`
- 是否走了 `downloads` 提供者
- 具体使用了 `query / insert / delete / openFile / openTypedAssetFile` 哪条链路

## 日志过滤

```bash
adb logcat -s FolderManager | grep "provider observe"
```

## 典型日志

```text
provider observe op=query authority=media pkg=com.tencent.mm attr=(null) uri=content://media/...
provider observe op=openFile authority=downloads pkg=com.tencent.mm attr=(null) uri=content://downloads/...
```

## 解释

- `op`：当前 ContentProvider 事务类型
- `authority=media`：MediaStore 链路
- `authority=downloads`：DownloadsProvider 链路
- `pkg`：调用方包名
- `attr`：AttributionTag / featureId
- `uri`：请求的内容 URI

## 注意

- 当前实现只做观测，不改写请求参数
- 这样做是为了避免再次引入微信白屏
- 如果确认链路后，再进入下一步做 Provider 侧过滤或改写
