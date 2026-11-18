# 升级接收、安装与存储接口

核对日期：2026-11-15。本文说明当前源码的接口、持久格式和恢复操作；正式产品要求见[系统设计](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)。资源与引脚统一见[硬件与运行资源](../../../../docs/hardware-resources.md)。

正常升级由 MQTT 通知、设备主动 HTTPS 下载，离线升级由 TF 文件导入；两者共用认证、安装和回退。USART2 仅用于 Boot 恢复，App 不开放串口配置或升级监听端口。HTTPS下载器和签名通知授权已接入源码；缺有效UTC、TLS或信任资料时仍拒绝，真实MQTT/BOS/安装及云端结果闭环尚待实板验证。

TF 维护由 PA0 长按至少 3 s、稳定松开触发。根目录仅有 /config.json 时交 Config 保存，仅有 /firmware.gwp 时导入升级；两者都有或都没有则拒绝。其他文件保留，插卡或启动不自动导入。缺信任资料、介质、正常启动身份或必要角色时拒绝擦写。

## 文件与职责

| 文件 | 职责 |
| --- | --- |
| shared/upgrade_package.c/.h | 同一套包头、ECDSA、GCM、明文摘要、认证后二次解密及向量检查 |
| shared/upgrade_baseline.h、upgrade_state.c/.h | 固定持久版本2，状态字段、转换、128 B编码、双副本/屏障读取和保护视图 |
| shared/upgrade_transaction.c/.h | 事务号高水位读取、预留与显式首次配置 |
| shared/upgrade_storage_protocol.h | M4/M3共用的请求/应答ABI，值拷贝，不是网络或Flash编码 |
| shared/upgrade_trust.c/.h | 只读可信密钥映射，默认未配置；不把包内数据作为密钥 |
| shared/upgrade_trust_flash.c | 读取Boot末尾128 B预留信任记录，拒绝无效记录，不包含密钥值或写入入口 |
| upgrade_stage.c/.h、upgrade_stage_auth.c/.h | 单会话接收、读回、固定期限、完整认证、提交状态和重启 |
| upgrade_storage.c/.h | M4侧真实队列请求、匹配/取消、旧源认证、Boot身份复核及READY提交解析 |
| upgrade_entry.c/.h、upgrade_tf.c/.h | 权限/操作epoch/会话归属/状态映射，以及固定TF路径的两遍导入 |
| services/storage/storage_upgrade.c/.h | Storage_Task中的7类请求、候选保护、事务预留及受约束READY提交 |
| services/storage/storage_upgrade_q.c/.h | 304/288 B静态值拷贝队列、迟到应答替换 |
| services/storage/storage_upgrade_tf.c/.h | 固定文件只读租约、open/read/close队列及拔卡失效 |
| upgrade_quiesce.c/.h | 全局变更门控、模块暂停/结算/同步、失败逆序恢复 |
| integration/app_upgrade、app_monitor | Boot交接、健康确认及任务进展/有限等待/栈余量监督 |

Boot保留独立Flash驱动和保存入口；普通构建具备实际安装及串口恢复路径。boot_recovery_receive由调用方传入可信映射，不从测试密钥头文件取得信任资料。

## 统一持久版本2

| 项目 | 固定约定 |
| --- | --- |
| GATEWAY_UPGRADE_PERSISTENCE_VERSION | 2；其他值在配置或编译时拒绝 |
| 安全包封装 | V2，必须具有经认证的IWDG_HANDOFF |
| manifest持久兼容号 | 2；版本1和未知值拒绝 |
| 队列协议 | 2，共用upgrade_storage_protocol.h |
| 普通/恢复日志 | GWSTATE2 / GWRCLOG2，128 B正文，扇区偏移256处另页提交 |
| 事务身份 | 非零tx_id、持久高水位、放行次数及操作类型 |
| 未确认试运行 | 最多两次已持久提交的放行机会，耗尽后回退或受控恢复 |
| App健康期限 | 总120 s、连续健康1 s |

包封装、持久兼容号及队列协议是不同字段，目前分别固定为2。Boot、App、打包和恢复工具使用同一约定，不保留持久版本1的执行分支；不支持的已提交记录仍拒绝，不当作空日志。

首次部署不承接开发阶段旧包、旧日志和业务数据。此决定不触发自动擦除或迁移；实板部署须明确初始化区域和信任资料保留范围。部署后的正常升级仍保护已确认恢复包及配置、未确认缓存、身份序列和控制去重状态。

## 真实接入顺序

1. App_Entry先读取Boot邮箱、尝试UpgradeTrust_Initialize、调用UpgradeStage_Init，再提前启动Monitor。信任资料缺失只使升级不可用，不伪造成功映射。
2. App_Entry只绑定一次UpgradeEntryPolicy，按来源分派到UpgradeTf_Authorize和NetworkUpgrade_Authorize。TF核对实体按键许可、执行任务和本次非零epoch；网络核对完整通知核验后的许可、网络执行任务及本次epoch。查询只读、无I/O，不将包字段或联网状态当作授权。
3. Storage_Init先创建共同协议的静态队列，Storage_Task恢复存储后调用UpgradeStorage_Bind。它检查运行上下文、协议版本、304/288 B消息尺寸及可信keys，再绑定Stage；旧296/20 B消息不会被当作新版。
4. M1/M2/M3分别绑定真实UpgradeQuiesceParticipant，并在最终TX/配置提交/持久变更前检查UpgradeQuiesce_AllowsMutation。四个角色为Modbus、Storage、Config、Network；关闭M2构建时不要求Network角色。
5. 各关键模块登记Monitor_RegisterHealth，按实际进展、有限等待和自检结果报告，并落实试运行门控；任务创建或接口注册不代替真实业务验证。
6. storage_upgrade_tf提供UpgradeTfSource：open取得不可变文件租约、read完整值拷贝、close幂等且确认后台不再引用缓冲，所有I/O转交Storage_Task。不因插卡/启动自动导入。
7. MQTT回调严格解析签名通知并计算有界SHA256，网络升级任务完成RTC及P-256校验后才调用UpgradeEntry_Begin/Write/Finish；回调不执行ECDSA、TLS或Flash。Boot串口恢复使用独立入口，不通过App收包接口。

NetworkUpgrade_Init通过NetworkUpgrade_BindAuthorization默认绑定真实P-256验签。通知先进入原单槽，网络任务核对RTC有效、目标匹配、签名正确和有效期后才发布本次epoch许可。Stage_VerifyNotice复用原操作锁、可信映射和分段验签，空闲以外返回BUSY，不擦写或分配事务号；不与TF认证并发。缺TLS、信任资料或有效UTC时拒绝。单槽在解析前占用，旧尝试结束并确认清理前不接下一条；epoch非零且本次启动不复用。

通知同时携带经包签名核验的image_sha256；与本次Boot已核验镜像相同的通知直接拒绝，不重复擦写或重装。通知许可同时绑定HAL单调截止时间，Write与Finish/提交前复查；到期失败沿原Disconnect结算。MQTT任务显式调用NetworkUpgrade_Revoke时锁存撤销，包括验签尚未完成的情况，不能在验签返回后重新授权。进入提交后仍按原规则解析结果，READY已持久不回滚，不等待云端回执无限推迟重启。

### 签名通知与M2下发

通知模式复用根目录tools/package_upgrade.py，无须设备AES或nonce账本，不重新制作包。通知过期只需重新签发通知，不能为了续期重置nonce账本或覆盖原包。先上传当前硬件匹配的.gwp到百度BOS，再签发通知；私钥留在制包电脑，M2只接收通知JSON和加密包。

在制包说明书同一个PowerShell终端的口令try/finally内执行下面整段命令；变量沿用制包流程。运行前把网关ID、任务号、BOS文件名改为本次实际值。示例V123不是已交付包，不能直接引用不存在的文件。

```powershell
# 1. 输入已制作并上传BOS的.gwp；这里只读取原包，不修改包或nonce账本。
# 2. 使用原签名私钥及临时口令环境变量，生成本次通知；同名输出拒绝覆盖。
& $gwPython $gwTool --notification `
  --input $gwPackage `
  --signing-key $gwPrivateKey `
  --signing-password-env GATEWAY_PACKAGE_SIGNING_PASSWORD `
  --gateway-id '实际网关ID' `
  --task-id 'ota-v123-001' `
  --url 'https://firmware.example.invalid/gateway-v123.gwp' `
  --valid-for 900 `
  --output (Join-Path $gwReleaseDir 'ota-v123-001.json')
# 3. 出错时停止；外层finally仍清除临时口令，不把口令交给M2。
if ($LASTEXITCODE -ne 0) { throw '签名通知生成失败。' }
```

| 项目 | 设备与工具共同约定 |
| --- | --- |
| 目标 | gateway_id与MqttClient_GetGatewayId相同：优先已置备身份，未置备时取本次运行配置mq_client_id；不是设备型号 |
| 主题 | 现有commands；百度模式为`$iot/{DeviceKey}/user/commands`；UTF-8原始字节、retain=false |
| 原文 | 固定字段顺序type/task_id/gateway_id/version/size/sha256/image_sha256/url/token/issued_at/expires_at/signature；无外部空白或转义，不重新排字段、格式化或再序列化 |
| 签名 | 可信编号1的P-256公钥；SHA256输入为ASCII `GWOTA-NOTICE-V1`、一个LF和删除signature字段后的完整紧凑JSON；签名64 B原始r||s编码为128个hex字符；V1只是通知域，包封装和持久兼容号仍为2 |
| 来源 | 仅`https://firmware.example.invalid/`的非空文件路径；URL≤127 B，不含空白或fragment；下载仍做TLS主机名和证书检查 |
| 时间 | UTC Unix秒，默认900 s、上限3600 s；issued_at不得晚于设备UTC，expires_at必须在未来；PC与RTC须正确校时 |
| 重复 | 同一次启动只接受递增issued_at；忙时拒绝。合法通知在设备重启后仍可在有效期内重新尝试，不等于跨重启持久任务去重；RAM会话不续传 |
| 输出 | 原包与签名私钥须匹配，生成前真实验原包签名，生成后真实验通知签名。可选`--token-env`从环境变量取下载认证头，不在命令行传明文token |

M2直接读取生成文件的原始内容发布，不需要私钥。受理回执仅表示通知排队，后续签名/时间失败通过ota_status报错；不能把受理或READY当作升级成功。工具打印原包的镜像SHA256，云端应随task_id、目标及发布记录保存，用于关联重启后的真实Boot结果；该结果仍使用tx_id/sequence/image_sha256，不伪造RAM中的task_id。

上述通知接入已实现，签名通知工具主机测试与构建可核对；真实MQTT通知、RTC/TLS峰值、BOS下载、网络安装和云端关联仍需联网实板验证。

耗时操作只由升级执行任务调用，不在Storage_Task、Modbus、MQTT、网络核心线程或Monitor执行。TF使用已装配的upgrade任务，静态栈6 KiB普通SRAM；初始化不重复创建任务，没有按键请求队列。Stage入口按当前PSP与任务栈底检查至少4096 B可用空间；历史最低余量由监督另查，两者不能混用。任务按实际文件/队列应答和密码检查点报告进展，阻塞前按原操作剩余期限声明等待，给返回处理另留1000 ms裕量；此裕量不延长收包、认证或提交的总期限。

## Config与TF配置接入

Config保留version=2基础参数，完整JSON增加mb_devices点表与write_allowlist。最多4站×16点、每站32寄存器；按256 B分块读取，不在app_config_t重复保存全点表。实际字段、限制和可编辑示例见[Modbus接口说明第15节](../modbus/Modbus_Interface_Guide.md#15-tf配置与启动加载)；该格式不是完整正式templates/devices/network schema。App串口不开放配置命令，厂商认证和本机网络参数仍由M2对齐。

PA0长按3 s后稳定松开，只检查根目录/config.json与/firmware.gwp。仅有前者时投递原Config任务保存，仅有后者时执行既有安全升级；两个同存或都没有拒绝。其他文件及记录目录不参与选择，不自动删除或导入。

启动先完整验证固定R的字段和CRC，再第二遍逐站填入原Modbus表，最后发布不可变运行配置；无配置不自动激活默认值。运行期间拒绝点表热改。TF导入先验证，再从同一稳定租约第二遍保存原文；CRC不符、读取失败或超时不提交，不改变当前R和最近S。

保存复用原三槽及15 s总期限；提交明确后提示重启生效。只保存校验后的TF原文，不在MCU重新编码JSON。Config阶段复用同一栈内值拷贝消息，文件/候选租约关闭或提交不明时继续占用，升级结算保持拒绝。没有新增任务、队列、锁或动态分配；TF文件服务复用原FIL并隔离配置/固件租约，其他路径不能读取或关闭它。

## 存储实际接入

Storage_Init只创建基础资源和初始化介质；Storage_Task恢复配置、序号、身份、时间下界、控制日志及TF只读视图，再分别上报资源和健康。控制日志的非零键读取两个保留区，同键异内容返回错误；满区不能覆盖已提交记录。尚未接通完整命令身份、持久执行与过期压缩，不代表控制事务已经完成。全空、读失败、已提交损坏分别处理。序号初始化只读，高水位须高于已恢复的控制/记录序号；首次分配同样先提交完整预留窗口。

RTC和时间下界统一采用1970年起的UTC Unix秒，RTC存UTC；初始化只读探测，上电/停振、非法BCD/日期或下界初始化失败均不视为可信时间。显式校时先完整写入24小时制时间并读回、清停振标志和重新上锁，状态与持久下界也成功才返回完成。软件检查不代替实际校准、I2C或掉电验证。

storage_runtime串行化完整业务操作，试运行或升级暂停时拒绝新变更；跨任务序号分配通过值拷贝代理转交Storage_Task。业务Flash和文件I/O使用固定操作期限，文件扇区同步轮询，返回前不遗留DMA引用。M2启用MQTT/Telemetry不改变存储独占要求，Config也通过原运行队列传值，存储端保持M3代码风格。

试运行只读检查TF现有卷、环状态、当前文件长度及最近记录格式/CRC，不建目录、段文件或写回。缺卡/无文件系统按未配置降级；读取失败和格式损坏不作为空白。自动写入自检默认关闭；正常缓存恢复也不因读取失败自动mkfs。

暂停先关闭准入，再等待在途完整操作退出；结算要求M2请求、配置事务、控制回收及TF租约均无待处理对象，否则BUSY并保留对象。同步由Storage_Task执行f_sync及环状态写入/读回。同步超时保留pending身份，后台未结束时恢复回调返回BUSY，不开放业务。

存储按实际I/O完成报告进展；等待声明不延长操作总期限，监督故障锁存后不恢复写入资格。暂停期间升级、取消和结算队列继续处理。App试运行总预算120 s，全部必要自检完成并连续健康1 s才请求确认；不要求外部网络在线或TF存在。

## 接收、取消与完成

```text
IDLE → STAGING → VERIFIED → QUIESCING → REBOOT_PENDING
          └─ 失败 → ERROR      └─ 未提交失败：恢复业务并清理
                              └─ 提交不明：保持暂停，只读解析
```

一次仅一个修改操作；并发修改立即BUSY，查询读取完整快照，不等待I/O。Stage函数表和上下文在绑定后保持有效，不重入修改接口。错误会话、越界或错误偏移不破坏当前会话；内容冲突、实际I/O失败、认证失败和到期终止本次接收。

| 操作 | 已实现行为 |
| --- | --- |
| Begin | 整包216～458960 B、来源和32 B传输SHA；成功才输出非零RAM会话号。prepare匹配本次Boot身份并完整认证confirmed，预留非零事务号，重新检查角色后才让M3准备另一槽 |
| Write | 同步读取1～1016 B输入，返回前须保持可读且不变；适配拆成≤256 B值拷贝入存储请求。完整写后逐字节读回，成功才推进；相同重复块只读比较、不重写、不推进或续期 |
| Finish | 重新读整包算传输SHA，再做实际公共认证/向量检查，成功只到VERIFIED并保留候选租约 |
| Commit | 必须VERIFIED；关变更门控，按模块暂停→结算→同步，全部成功才请求持久READY，复读确认后到REBOOT_PENDING |
| Abort | 未提交时幂等释放占用，不擦包或日志；UNKNOWN/已提交READY拒绝撤销。恢复或关闭失败分别记录并保留门控/租约 |
| RequestAbort | 只置取消标记，认证/分块在检查点停止；不代表后台已结束。等待应答每片最多250 ms复查，但仍须等取消应答确认占用释放 |
| GetStatus | 会话/事务/槽/长度/偏移/阶段、首因、关闭和恢复错误、忙状态及提交处置；只做RAM到期检查，不执行存储或重启 |
| Poll/Reboot | Monitor只标记到期/取消，不做存储关闭；READY明确持久后给传输1 s发送响应，再复位兜底。调用方也可在成功响应发送后主动Reboot |

prepare总300000 ms、30 s无新增读回数据、Finish传输摘要及完整认证共180000 ms、Commit暂停/同步及首次提交共30000 ms，均不随进展续期。超时后解析未知提交可另给3000 ms只读窗口，不能重发写请求；失败恢复和关闭各自有界，不把它们当成原操作成功。

首次失败保存在error，close_error对应Stage的cleanup_error，业务恢复失败另存resume_error；不能用后续清理成功覆盖首因。未知提交保持QUIESCING，不恢复业务、不复用候选；下一次Commit只读核对实际日志。原基线仍完整才可判未提交并恢复，匹配本次READY才可重启，损坏/读失败继续保留诊断。

## 网络入口与TF导入

App正常收包仅接受NETWORK（2）和TF（3），旧UART来源值1被拒绝；Boot恢复不经过Stage。已删除固定监听端口46000、300 s窗口及OpenWindow/CloseWindow/WindowOpen/WindowRemainingMs接口，没有常开端口或默认放行授权。网络接口只表达本次下载操作，不绑定HTTP连接、MQTT连接或云端任务号。

UpgradeEntryPeer为来源和本次启动不复用的非零epoch。M2每次下载尝试分配新epoch，同一次尝试保持不变；云端任务去重及任务号与peer/session的对应由M2管理，不在M4重复建表。Write/Finish/Abort/Reboot须与owner及session匹配，不能用新的下载尝试继续写旧会话。下载中断、主动取消或撤权时用原peer调用Disconnect；仅MQTT状态连接短暂断开而下载仍有效，不等同下载中断。

Entry维护权限和操作归属，Stage维护会话进度、存储占用和提交状态。Write/Finish/Abort/Reboot共用TakeSession检查。Begin先由RetirePreviousSession处理旧会话，查询或清理失败保留原owner；成功后预占新owner，再次检查权限，撤权通知可以锁存取消。Begin成功才输出会话号，失败且未清理时仍保留归属。GetStatus只读，但也必须通过当前权限检查。

M2调用顺序固定如下，不需要额外的M4云端API：

| 步骤 | 接口与约定 |
| --- | --- |
| 准备 | 登记可信peer；Begin(peer, NULL, total_size, sha256, &session_id)，长度为完整加密包B数，摘要可读32 B；失败不继续写 |
| 下载 | 按偏移调用Write，每次1～1016 B；返回后可复用缓冲，30 s没有新增读回数据会到期，不支持中断后的跨会话续传 |
| 完成 | 全部字节写入后调用Finish；OK仅表示READY已持久及准备重启，不能上报安装成功 |
| 取消 | 使用原peer调用Disconnect，即使权限已撤销也可取消；BUSY表示已请求但未结算，执行任务稍后查询/重试；UNKNOWN或READY不得当成可撤销 |
| 重启 | READY明确后可调用Reboot，另有1 s自动重启兜底；不能等待云端应答无限延迟重启 |
| 最终上报 | M2根据重启后真实确认/回退状态上报；任务与安装结果的跨重启关联仍须在平台接入时落实，当前RAM状态不承担该职责 |

两种取消标志仍分开：owner_cancelled记录连接断开，覆盖Stage尚未分配会话号的准备期；cancel_requested针对已发布会话，在检查点结束操作。已知会话号时，锁存断线与请求Stage取消在同一短临界区完成；这里只操作RAM，不执行存储、认证或用户回调。发布BEGIN结果也不重置断线标志。Disconnect取得入口后重新核对peer，旧连接不能释放新连接的归属。

入口异常处理约定：

| 场景 | 核对的处理路径 |
| --- | --- |
| 初次状态查询或清理后查询失败 | RetirePreviousSession立即返回，不继续准备或替换owner |
| BEGIN尚无会话号时断线 | Entry锁存；若Stage已发布则请求取消，否则BEGIN返回后补做Abort；失败不输出成功会话号 |
| 新连接抢占活动会话 | 返回BUSY；旧连接已断开时必须先由Stage确认清理完成 |
| close或resume失败 | 保留原owner与错误；原连接或已断开会话可重试清理，不交给新连接继续写 |
| 旧Disconnect延迟返回 | 取消目标使用捕获的会话号；查询和取得入口后重查连接身份，不清除后来的owner |
| 未绑定策略、权限被拒或来源为旧UART/非法值 | 不进入存储准备；Stage自身也拒绝旧UART来源 |
| 清理期间撤权 | 旧会话清理结束后登记归属并复查权限；拒绝时释放新归属，不开始新包 |
| 提交结果UNKNOWN | 保持QUIESCING；Begin/Abort拒绝，原连接重复Finish仅只读解析，不重发写入 |
| 同会话重复Finish | VERIFIED跳过再次认证并继续Commit；QUIESCING只读解析；REBOOT_PENDING直接返回OK |

UpgradeEntry_Finish执行Stage_Finish和Stage_Commit；返回OK仅表示READY已持久/准备重启，安装和健康结果须重启后查询。ErrorName/PhaseName给M1/M2统一的E_FORMAT/E_RANGE/E_MODE/E_BUSY/E_STORAGE/E_INTEGRITY/E_AUTH/E_TIMEOUT/E_INTERNAL及阶段名称，不另做JSON实现。

TF只接受/firmware.gwp。UpgradeTf_Import先对不可变租约文件计算整包SHA，再BEGIN、第二遍读文件并逐块调用同一Write，关闭文件后Finish/Commit；文件变化会被最终持久摘要/认证拒绝。每遍文件处理固定180 s，不在Storage_Task内同步排队等待自己。文件关闭错误和包槽取消错误分别返回，关闭未确认先隔离旧租约；RequestCancel只请求取消，不伪造完成。

PA0每20 ms轮询，按下/释放均消抖40 ms；从确认按下起计时，达到3 s后仍须释放才触发，持续按住不触发。启动及导入返回后先等待稳定释放，避免旧按压再次启动升级。每次触发先核对正常Boot身份、后端和模块回调，再使用非零且本次启动不复用的epoch调用导入；忙或缺依赖当次拒绝，不排队。失败关闭和取消分别记错，未释放占用时下次按键先重试清理；提交不明继续保持原保护。USART1仅输出拒绝原因或导入结果，不承担触发。

元数据与包内容读取共用ReadResponse，在WaitResponse核对请求号、类型、会话、事务和期限后，再要求actual_len恰等请求长度才复制。地址/已写范围和1～256 B限制仍在各调用方；元数据保留首次I/O错误。短读或超长应答返回INTEGRITY，诊断也记录失败。没有合并写入、提交或关闭流程。

## 日志布局和只读保护

下列地址为外部Flash字节偏移，区间右端不包含。M4共享代码使用相对UPGLOG基址0x380000的偏移；App始终由M3所有者映射，Boot独占自己的驱动。

| 用途 | 物理区间 | 相对偏移 |
| --- | --- | --- |
| 普通状态双副本 | [0x380000,0x382000) | 0x0000、0x1000 |
| 恢复状态双副本 | [0x382000,0x384000) | 0x2000、0x3000 |
| 恢复启动屏障 | [0x384000,0x385000) | 0x4000 |
| 基线2事务号高水位双副本 | [0x385000,0x387000) | 0x5000、0x6000 |
| 其余UPGLOG | [0x387000,0x3C0000) | 保留，不自动扫描或格式化 |

每个副本一个4 KiB扇区，128 B本体，扇区偏移256处另页4 B提交。普通CMIT、恢复RCMT，CRC-32/ISO-HDLC覆盖本体前124 B。记录不能直接按C结构体内存写入。

| 本体偏移（B） | 长度 | 基线2普通状态 | 基线2恢复状态 |
| --- | --- | --- | --- |
| 0 | 8 B | GWSTATE2 | GWRCLOG2 |
| 8 / 12 / 16 | 各4 B | 版本2 / 非零sequence / 普通phase | 版本2 / 非零sequence / 恢复phase |
| 20 | 40 B | confirmed包描述 | 恢复package描述 |
| 60 | 40 B | candidate包描述 | 60～67 tx_id，68～71次数，72～75操作，76～99为0 |
| 100 | 8 B | tx_id | 0 |
| 108 / 112 | 各4 B | trial_attempts / operation | 0 |
| 116 | 8 B | 0 | 0 |
| 124 | 4 B | CRC32 | CRC32 |

包描述为槽号u32、完整加密包长度u32、32 B明文SHA；槽0不存在、1/A、2/B。u32/u64均小端。operation为1 ENROLL、2 INSTALL、3 RESTORE、4 CANCEL、5 RECOVERY。普通CONFIRMED无candidate，保留最近事务/操作/次数；初始建档ENROLL/0次，新READY为新事务INSTALL/0次。恢复事务用RECOVERY，转普通CONFIRMED复制同一身份和次数，先普通提交→恢复DONE→最后清屏障。

只编码和接受上述版本2日志。日志格式与安全包format_version是独立概念；旧格式或未知格式返回UNSUPPORTED，不当作空白或自动改写。

Load读取双副本和提交标记，选较大不回绕sequence。同序号、任一已提交副本损坏及有残留但无有效副本均拒绝；未提交残片与有效旧副本并存时可忽略残片，必要读取失败不退回旧副本。CRC有效但另一格式返回UNSUPPORTED，不能当EMPTY。结果原0～4保留，UNSUPPORTED追加为5；CRC不提供认证或整片快照防回放。

恢复Load先看屏障首字节，FF才EMPTY；屏障激活但无有效记录为CORRUPT。原始LoadRecords供显式恢复前保护/保存，不绕过屏障启动；CONFIRMED/DONE恢复源在没有另一可信源时都受保护。

ReadProtection只给元数据快照：普通CONFIRMED、恢复EMPTY才返回未引用的另一候选槽；双EMPTY、活动事务、读取错误或损坏都不授予普通可写槽。候选位图不代替旧源认证、Boot身份匹配和从查询到准备期间的独占/预期视图检查。

## 事务号与试运行

sequence每次记录提交递增，tx_id跨安装、重做、试运行和回退保持；同一包再次下载也须新事务号，不能取HAL tick、RAM session或包摘要。M3旧pkg_id为SHA256(manifest)前8 B关联值，与包头16 B发布package_id不同；共同启动状态使用完整角色描述及tx_id，不把截断值当认证身份。

高水位每份128 B：GWTXID02、u32版本2@8、非零generation@12、u64 last_allocated@16、24～123全0、CRC@124，TXMT提交@256。Reserve先提交另一副本递增值并复读成功才返回ID，失败输出0；未知提交视为可能已消耗，允许跳号、不回收，不回绕。新代次的高水位倒退也判损坏。

Initialize只由显式首次配置调用，必须全部[0,0x7000)升级元数据为FF，才写generation=1/last_allocated=0；不擦除任何残留。启动、BEGIN和恢复不自动初始化；已有事务却丢失高水位不能归零。Boot每次分配/首次配置固定10000 ms总期限。首次配置通过维护帧0x30的initialize_transactions独立载荷发起，工具选项为--initialize-transactions；非空元数据不得借此归零。

已存在可信事务的续装/回退不需分配新ID，新恢复需要有效分配记录。高水位丢失须从可信留档维修，不能自动重置；不复用保证以介质未被人为回滚为前提，CRC不是安全单调硬件。首次置备或维修必须先明确待初始化区域与要保留的资料；正常升级不执行此操作。

| 基线2试运行事件 | 已实现处理 |
| --- | --- |
| 安装及片内校验完成 | 先保存TRIAL/次数1，再发布邮箱/跳转 |
| 次数1有匹配确认 | 重认证并核对片内App后CONFIRMED |
| 次数1无确认 | 源包/App仍有效才保存TRIAL/次数2，再放行 |
| 次数2有匹配确认 | 同样复核后CONFIRMED |
| 次数2仍无确认 | 普通RESTORING并保留ID/次数，恢复旧包成功才清candidate |
| 任一次认证/片内检查失败 | 不为凑足次数执行损坏镜像，走回退/受控恢复 |
| 独立恢复没有可信旧源且耗尽 | BLOCKED并保留屏障，等待明确维护 |

次数为已提交的放行机会；写记录后、跳转前掉电也计入。禁止次数减少或同事务重新归零；每次放行有不同sequence。64 B邮箱基线2在word12/13放tx_id低/高32位，word14放次数；Boot仅软件复位且域/事务/序号/摘要/次数匹配才接受意图，仍重认证及比对App。

App从入口起最长120 s，可信平台/产品自检连续健康1 s才请求确认，不要求外部网络在线或TF存在。Monitor聚合各关键模块登记/真实进展/自检，合法等待不能续期，过期或致命平台故障锁存停止喂狗；确认前再次原子检查，避免Poll后故障仍写确认。缺实际报告不自动确认，不能称整机试运行已验收。

### 启动结果与联网交接

现有升级任务通过App_UpgradePollResult最多每秒尝试一次只读核对，每次总限3 s，成功取得本次启动快照后停止读取；读失败保留重试。UpgradeStage_ReadBootState与收包共用串行占用，仅空闲或已清理ERROR允许；实际读取仍由Storage_Task处理既有READ_METADATA消息，不直接访问Flash。收包中、提交不明、旧占用未释放、读失败或身份不匹配均不产生成功结果。

| 本次Boot及持久状态 | 结果 |
| --- | --- |
| 普通/恢复TRIAL且事务号、序号、次数、镜像摘要匹配 | TRIAL，保持试运行门控 |
| NORMAL与CONFIRMED/INSTALL或RECOVERY全部匹配 | CONFIRMED |
| NORMAL与CONFIRMED/RESTORE全部匹配 | ROLLED_BACK |
| 缺依赖、读取失败、身份不符或首次ENROLL/CANCEL | UNKNOWN，不报终态 |

已知结果为本次启动的只读快照，健康确认仍须复位交Boot认证和持久提交。原App_UpgradeGetBootIdentity仍只允许正常启动；新增StartupIdentity只用于结果关联，不开放业务或收包权限。

网络升级任务空闲时读取RAM快照，MQTT连通后发布ota_status；不在MQTT或监督任务中等待存储。事件type为boot_result，含16个小写hex字符的字符串tx_id、u32 sequence、phase（trial/confirmed/rolled_back）及64个小写hex字符的明文image_sha256（32 B）。UNKNOWN保持待判定。sequence不是云端任务号，image_sha256也不是下载通知的整包传输SHA；云端task_id关联和按事务/序号/阶段去重由M2及云端处理。

启动结果复用原QoS1槽和发送锁；重复调用不叠加在途副本，匹配PUBACK才结束本次待报。断线时撤销连接内packet_id，未确认结果以原始身份重发，已确认结果在本次启动内不重发。分配编号跳过仍在途的ID。PUBACK只证明broker接收，不冒充云端任务处理回执；重新上电可能再次发送相同快照。不新增任务、队列、持久格式或擦写步骤。

IWDG当前为256分频、4095重装值。2026-11-15试运行故障测试观察约33.86/33.91 s后复位并回退；当前阶段已明确放宽原8 s目标、优先完成流程。软件期限不等于所有工况下的硬件复位上限。

## M4/M3队列契约

唯一消息定义为firmware/shared/upgrade_storage_protocol.h；日志编解码及提交规则复用upgrade_state.c/.h、upgrade_transaction.c/.h。M3在现有Storage_Task适配，不另建Flash所有者。

实际请求为0 BEGIN、1 CHUNK、3 ABORT、4 READ_PACKAGE、5 READ_METADATA、6 RESERVE_TX、7 COMMIT_READY。值2保留，返回INVALID且不执行I/O；M4不发送FINISH，不新增CRC完成流程。Entry/Stage Finish是认证和提交协调，不是旧队列FINISH。队列槽号双方均0/A、1/B，M4状态槽号1/A、2/B已有转换。

| 操作 | 输入、输出与边界 |
| --- | --- |
| RESERVE_TX | 会话前持久预留ID，完整提交成功才返回非零tx_id |
| READ_METADATA | session/tx_id为0，offset和1～256 B长度；基线2范围[0,0x7000)；短读不是空日志 |
| READ_PACKAGE | 候选须匹配存储session/tx_id且不越已写范围；已确认源允许准备前读，但仅有效角色和包长度范围 |
| BEGIN | 包长、候选槽、预留ID及预期普通/恢复视图；M3执行时重新检查并独占后才擦候选 |
| CHUNK | 连续偏移、1～256 B值拷贝，首块1 B也有效；完整持久后才成功，M4再逐字节读回，不写DOWNLOADED或释放认证占用 |
| ABORT | 已知session按会话；BEGIN应答丢失则session=0、按非零tx_id；幂等，不擦包/日志，session_ended确认结束 |
| COMMIT_READY | 已认证及QUIESCING完成的128 B普通READY/INSTALL本体；检查角色、事务、次数0、预期视图和序号，完整落盘/复读后成功 |

COMMIT_READY不能提供任意日志写入。confirmed不变、candidate与租约匹配；新READY的sequence必须为旧+1。当前READY与本体逐字段相同且恢复前置满足时只读幂等确认，不新写；未知结果先解析，不盲目重发。已提交候选持续受保护，RAM租约关闭不允许擦它。

| 请求偏移（B） | 类型/字段 |
| --- | --- |
| 0 / 4 / 8 / 12 | u32 req_id / session / offset / pkg_len |
| 16 | u64 tx_id |
| 24 / 28 | u32 expected_normal_sequence / expected_recovery_sequence |
| 32 / 36 | u32 expected_normal_result / expected_recovery_result |
| 40 / 42 / 43 | u16 len / u8 type / u8 slot |
| 44～299 | u8 data[256] |
| 300～303 | u32 deadline_ms：HAL绝对期限，出队及分段修改前检查，不续期 |

| 响应偏移（B） | 类型/字段 |
| --- | --- |
| 0 / 4 | u32 req_id / session |
| 8 / 12 | i32 result / u32 written |
| 16 | u64 tx_id |
| 24 / 26 / 27 | u16 actual_len / u8 session_ended / u8 type |
| 28 | u32 detail，底层诊断，无错误时0 |
| 32～287 | u8 data[256] |

ARM EABI下请求304 B/data@44、响应288 B/data@32，头文件有静态断言。未用字段清零、无指针；消息只供CPU时可按已审查布局放置，DMA实际缓冲须普通SRAM。深度1不自动保证单请求在途，M4服务仍串行修改。

result固定0 OK、1 INVALID、2 BUSY、3 IO_ERROR、4 TIMEOUT、5 CORRUPT、6 UNSUPPORTED、7 CONFLICT、8 NOT_READY，禁止直接强转HAL或日志结果枚举，HAL诊断存detail。expected_*_result使用UpgradeStateResult的0～5值，不是队列结果的0～8值。M4严格匹配req_id/type/session/tx_id，BEGIN新session和RESERVE_TX新ID为成功返回值例外；READ成功长度必须恰等请求，失败不使用部分数据，丢弃迟到应答不续期。

UpgradeStorage_GetDiagnostic只读返回当前/最近请求号、类型、偏移、槽、事务、映射结果及匹配应答的原始状态/detail/实际长度；不把旧应答detail冒充本次失败。它不访问队列、不更新进展，也不包含包内容、明文或密钥。

Storage_Task须有界唤醒/连续服务并保留控制/采样公平性，不能每个256 B请求固定延时1000 ms。最大包每遍至少1793次读取，固定每次1 s已超过180 s认证预算。吞吐需实板测量，不能只增加设备超时掩盖调度问题。

## 信任资料、工具与启用

F407 Boot/App由根CMake自动接入shared/upgrade_trust_flash.c，覆盖默认拒绝映射的弱实现；GATEWAY_UPGRADE_TRUST_SOURCE非空时使用指定的其他保护区实现。两边取得运行期间不变的65 B SEC1公钥和32 B AES只读指针，当前批准密钥编号均为1；没有有效记录时Initialize返回false，升级保持NOT_READY。

Boot的64 KiB分区末尾[0x0800FF80,0x08010000)独立保留128 B信任记录，链接器将Boot代码/常量/初值限制在前65408 B。App仍从0x08010000开始，容量448 KiB；普通升级安装器只擦写App区域。记录单独置备，不编入通用ELF/bin。空白、格式、CRC、公钥或AES校验失败均拒绝，不从待验包、TF或配置文件取得替代密钥。

信任记录固定128 B，整数为小端，不直接写C结构体；工厂/维护资料按下表准备，不放进通用构建产物。

| 相对偏移（B） | 长度（B） | 内容与校验 |
| --- | --- | --- |
| 0 | 8 | ASCII GWTRUST后跟一个0x00字节 |
| 8 | 4 | 记录总长度128 |
| 12 / 16 | 各4 | 签名/解密密钥编号，均为1 |
| 20 | 65 | SEC1未压缩P-256公钥，须前缀0x04且为有效曲线点 |
| 85 | 32 | AES-256密钥，拒绝全0x00或全0xFF |
| 117 | 7 | 保留字段全0x00 |
| 124 | 4 | CRC-32/ISO-HDLC覆盖前124 B，小端存储 |

CRC只检测意外损坏，不替代置备来源的可信性。Boot重写会擦到同一扇区中的信任记录，必须在同次维护中重新置备并读回；日常App下载和正常升级应保留Boot。WRP/RDP等保护设置在安全评审与恢复验证后单独冻结，开发阶段可读写不等于已完成生产保护。生产密钥和私钥不得进入Git、源码、通用ELF/bin、日志、TF或导出。

现有打包工具支持生成相同布局的受控记录；验签公钥为PEM P-256，设备AES文件为32 B。示例在工程根目录的PowerShell终端执行，所有路径需换成已受控保存的真实文件：

~~~powershell
python tools/package_upgrade.py --aes-key D:/GatewaySecrets/device-aes.bin --public-key D:/GatewaySecrets/signing-public.pem --trust-record D:/GatewaySecrets/boot-trust-record.bin
~~~

目标必须不存在且位于Git工作目录外；输出包含设备AES密钥，不放入TF、发布包、源码或日志。该命令只生成128 B记录，不连接板子。首次置备按目标板核验资料，先写Boot，再将记录写入0x0800FF80并读回核对；不擦除OTP或自动设置保护选项。

正常加密包继续使用独立nonce账本及--iwdg-handoff；--trust-record不初始化或消耗账本，也不接收签名私钥。记录的128 B布局和包格式不因存放位置变化而改变。工具自检、文件置备和板端认证安装分别验证，文件生成成功不代表设备已接受该信任资料。

tools/package_upgrade.py读取外部受控P-256私钥及设备AES文件，生成V2加密包，持久号默认且只允许2，明确固件版本并确认实际App接管IWDG。AES须为32 B且不能全为00或FF，与设备信任记录准入一致；CLI及账本初始化、nonce预留、制作包接口均先校验，无效密钥不创建或修改账本。当前硬件标识仍为源码批准的F407-TEST；此标识不表示使用测试信任根。输入必须为0x08010000偏移且向量有效的App，不写出明文/密钥副本，输出前真实验签及GCM自检。PEM密码可从指定环境变量读取，不把密码放命令行。

nonce由每AES密钥唯一的SQLite账本预留，先持久递增再加密，失败消耗的值不复用；同账本并发受数据库事务串行。首次登记才用--initialize-ledger，不覆盖已有密钥记录；正常制作不自动新建丢失账本。每key只能有一个持续维护的账本，不能复制后并行用；丢失或回滚须轮换AES密钥，不能重置同key账本继续发行。该保证依赖可信发布环境及账本可靠保管。

tools/boot_recovery.py的--persistence-version默认且只允许2；发送前核对包和设备INFO，要求mode=BOOT及compat=2，缺字段或其他值拒绝。--initialize-transactions仅在INFO显示首次空白时明确请求首次初始化；设备还完整检查全部元数据FF，失败不擦除重试。旧文本发送脚本不适用于当前Boot；首次部署使用显式事务号初始化和同一恢复流程，不绕过认证或试运行。

### Boot恢复维护帧

Boot及tools/boot_recovery.py复用maint_frame V1：A5 5A、version=1、opcode、flags、request_id(u32LE)、payload_len(u16LE)、载荷及CRC32LE。载荷最多1024 B，CRC覆盖version到payload；请求flags=0，响应flags=1并回显同一请求号。Boot正常启动日志不属于协议应答，工具按帧和请求号识别结果。

| 命令 | 请求载荷 | 结果 |
| --- | --- | --- |
| 0x01 INFO / 0x02 STATUS | JSON `{}` | BOOT模式、compat、普通/恢复状态、保护槽、高水位状态及当前会话进度 |
| 0x30 RECOVERY_BEGIN | JSON `{"slot":2,"total_len":985,"allow_unknown_state":false}` | 保护检查、提交恢复屏障及擦槽后返回session_id、slot、total_len、next_offset=0；slot为1/A、2/B，allow_unknown_state可省略且默认false |
| 0x31 RECOVERY_CHUNK | session_id(u32LE)、offset(u32LE)、data(1～1016 B) | 顺序分块写入并读回，返回next_offset；工具默认每块256 B |
| 0x32 RECOVERY_FINISH | JSON `{"session_id":7}` | 收齐并完整认证、持久提交READY后返回ACCEPTED/READY；安装器运行一次，再以同请求号返回OK/TRIAL及install_result=1，失败返回ERROR |
| 0x16 REBOOT | JSON `{}` | 无活动收包时回应OK后复位；工具--run使用此命令，重启由Boot重新判断持久事务，不宣称App已成功运行 |

原有“首次配置事务号”操作复用0x30的独立载荷`{"initialize_transactions":true}`，不能与收包字段混用、不建立收包会话。此操作由工具--initialize-transactions明确发起，不自动擦除或初始化已有元数据，不新增命令号。请求仅支持表中字段与类型，重复字段、溢出、尾随内容和未知字段拒绝。

响应统一JSON：request_id/status/error/field/restart_required及表中结果字段。失败采用E_FORMAT/E_REQUIRED/E_RANGE/E_MODE/E_AUTH/E_BUSY/E_STORAGE/E_INTEGRITY/E_VERSION/E_TIMEOUT/E_EXPIRED/E_CAPACITY，不返回密钥。ACCEPTED仅表示READY已持久；OK/TRIAL表示安装器已登记试运行，仍须实际跳转与App健康确认。

单会话；session_id复用持久事务号的u32可表示值，不在重启后复用旧编号，超过范围返回E_CAPACITY且不擦包槽。同请求号且参数相同的活动BEGIN返回原会话，不再次擦除；参数冲突拒绝，活动时其他BEGIN返回E_BUSY。CHUNK重复范围须完全落在已确认偏移内，逐段读回内容相同才返回原偏移，冲突拒绝，不允许部分重叠续写。重复BEGIN、重复块及查询均不延长接收进展期限。

半帧2 s超时丢弃，CRC/长度错误重新同步；30 s无新增写入则结束RAM会话，保留恢复屏障和半包，禁止FINISH。收包总期限180 s从擦槽完成后计算；擦槽仍有300 s总期限，认证与安装沿用原有监督。会话到期或写入失败后须用新BEGIN显式重试，不能用旧会话继续；工具不自动重试擦写。断电后不凭RAM偏移续传。

Boot恢复使用**USART2 PD5/TX、PD6/RX，115200 8N1，无流控，轮询收发**。恢复收发使用huart2，跳转App前先关闭USART2并释放PD5/PD6，再关闭日志串口及恢复时钟。USART1 PA9/PA10继续输出日志。

USB转串口使用3.3V电平：RX接PD5、TX接PD6、GND共地；选择这一路适配器的COM口。关闭维护口串口监视器，工具独占该口，打开工具后复位设备；如需同时看日志，可另开USART1的监视器。命令在工程根目录执行：

先查询状态；以下示例的COM7必须换成实际维护口。每次先启动工具，再在其等待期间复位进入Boot维护窗口。

```powershell
# 第一步：只读查询，确认保护槽、持久版本及当前会话。
python tools/boot_recovery.py --port COM7 --info
```

只有首次部署、INFO显示空白且已确认初始化范围时才执行下一块；已有事务时跳过。

```powershell
# 第二步（仅首次空白建档）：显式初始化事务号；不擦除旧数据。
python tools/boot_recovery.py --port COM7 --initialize-transactions
```

将包路径和槽号换成实际值；下面B仅为示例，必须是允许擦写的候选槽。

```powershell
# 第三步：发送合法加密包并安装；此命令会擦写指定候选槽。
python tools/boot_recovery.py --port COM7 --package 'C:/实际目录/gateway.gwp' --erase-slot B
```

```powershell
# 第四步：需要离开维护状态时执行；重启后Boot重新判断持久状态。
python tools/boot_recovery.py --port COM7 --run
```

工具与Boot均保护可信已确认源。工具超时表示结果未知，应先查询，不自动擦除重试。安装OK/TRIAL之后仍需核对App实际运行、健康确认及持久结果，不能只看发送完成。

## 已有验证及限制

2026-11-14～15 的外接 W25Q64、SPI3/分频8、冻结 V121/V122 实板记录覆盖：

- USART2 合法恢复、V121→V122 的 TF 按键升级、安装、健康确认及再次复位后保留确认状态。
- 恢复协议错误请求、重复块、超时、软件复位屏障及合法重试；TF 坏签名拒绝、取消清理和原版本再次启动。
- 候选两次试运行故障后 IWDG 复位并恢复已确认槽。本轮两槽均为 V122，证明槽和事务回退，不证明不同版本业务兼容。
- 安装擦除后、首次试运行确认前、TF 部分收包、Boot 恢复部分收包四个阶段各一个实际断电点。复电后的重装、重试或保持恢复屏障符合预期；恢复收包中断后用合法包重新完成安装确认。

主机共用认证负例 44 项通过，不等于全部坏包实板覆盖。硬件记录针对冻结镜像；2026-11-15 目录整理后的构建未重新烧录。四个断电点不等于覆盖所有写指令边界或整机随机断电矩阵。

仍需完成真实通知来源/目标/时效授权及绑定、云端任务关联、有效 UTC 下的 HTTPS/BOS 下载和握手内存峰值、网络升级与最终回执。TF 配置导入、真实从站与存储/网络并发、完整控制持久执行及长期稳定性也不能由上述升级测试代替。详细现场证据保存在本地测试记录，日常资源数字只维护在硬件与运行资源文档。
