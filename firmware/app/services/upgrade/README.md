# M4统一升级接收、安装协调与存储接口

M4提供共用包认证、持久日志、事务号、Boot安装/回退、App接收、队列适配、网络升级入口、TF导入、暂停结算和监督。正式资料入口为[系统设计](https://hcn4ticlccda.feishu.cn/docx/JNMDdxoBio9f9bxxQPXcPINSnqh)。2026-11-10已同步正式需求、设计和分工：正常升级由云端通知、设备主动下载，不再依赖串口开窗；本轮统一持久版本2，文档修订号分别为58、45、97（写后已读回）；此前已同步设计架构图。

串口保留首次配置和Boot故障恢复，TF保留离线升级。M2负责MQTT任务通知、下载来源与权限校验、文件下载及结果上报；M4只接收包数据并协调认证/安装，不新增Socket监听、云平台、下载器或任务队列。HTTPS为拟选下载方式，具体认证、资源及云端接口由M2核对，不能把本目录改动称为云端升级已接通。

2026-11-10已把M4可独立实现的代码写入工程；存储端及TF服务现已按同一契约写入，并在Storage_Task内接入绑定调用；Storage任务已启用并提供存储侧暂停/结算/同步和健康报告，真实权限及其他模块回调仍未齐备，默认BEGIN仍为NOT_READY。没有用成功桩开放擦写，没有新增任务或直接访问App的FatFs/SPI，也没有操作板子。源码存在、正式方案已同步与真实升级闭环是不同交付状态。

## 文件与职责

| 文件 | 职责 |
| --- | --- |
| shared/upgrade_package.c/.h | 同一套包头、ECDSA、GCM、明文摘要、认证后二次解密及向量检查 |
| shared/upgrade_baseline.h、upgrade_state.c/.h | 固定持久版本2，状态字段、转换、128 B编码、双副本/屏障读取和保护视图 |
| shared/upgrade_transaction.c/.h | 事务号高水位读取、预留与显式首次配置 |
| shared/upgrade_storage_protocol.h | M4/M3共用的请求/应答ABI，值拷贝，不是网络或Flash编码 |
| shared/upgrade_trust.c/.h | 只读可信密钥映射，默认未配置；不把包内数据作为密钥 |
| upgrade_stage.c/.h、upgrade_stage_auth.c/.h | 单会话接收、读回、固定期限、完整认证、提交状态和重启 |
| upgrade_storage.c/.h | M4侧真实队列请求、匹配/取消、旧源认证、Boot身份复核及READY提交解析 |
| upgrade_entry.c/.h、upgrade_tf.c/.h | 权限/操作epoch/会话归属/状态映射，以及固定TF路径的两遍导入 |
| services/storage/storage_upgrade.c/.h | Storage_Task中的7类请求、候选保护、事务预留及受约束READY提交 |
| services/storage/storage_upgrade_q.c/.h | 304/288 B静态值拷贝队列、迟到应答替换 |
| services/storage/storage_upgrade_tf.c/.h | 固定文件只读租约、open/read/close队列及拔卡失效 |
| upgrade_quiesce.c/.h | 全局变更门控、模块暂停/结算/同步、失败逆序恢复 |
| integration/app_upgrade、app_monitor | Boot交接、健康确认及任务进展/有限等待/栈余量监督 |

Boot保留独立Flash驱动和保存入口；普通构建也具备实际安装及串口恢复路径。boot_package_receive不再包含测试密钥头文件，调用方传可信映射。测试根只由BOOT_PACKAGE_RECEIVE_TEST显式绑定，正式映射与该开关禁止同时选择。

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
| App健康期限 | 总30 s、连续健康1 s |

包封装、持久兼容号及队列协议是不同字段，目前分别固定为2。Boot、App、打包和恢复工具使用同一约定，不保留持久版本1的执行分支；不支持的已提交记录仍拒绝，不当作空日志。

首次部署不承接开发阶段旧包、旧日志和业务数据。此决定不触发自动擦除或迁移；实板部署须明确初始化区域和信任资料保留范围。部署后的正常升级仍保护已确认恢复包及配置、未确认缓存、身份序列和控制去重状态。

现有标准预设均明确选2，重新配置会更新旧缓存；历史build/M4V2*目录不再代表另一种可选基线。BootDebug保留调试信息并使用-Os以满足64 KiB；AppDebug优化方式不变。

## 真实接入顺序

1. App_Entry先读取Boot邮箱、尝试UpgradeTrust_Initialize、调用UpgradeStage_Init，再提前启动Monitor。信任资料缺失只使升级不可用，不伪造成功映射。
2. 可信装配绑定UpgradeEntryPolicy。authorize按来源和操作epoch检查实际权限，回调只读、快速且可并发查询，不从请求JSON读取布尔量当授权。M2先验证云端任务及下载来源，再登记本次epoch；撤权时同时调用Disconnect。
3. Storage_Init先创建共同协议的静态队列，Storage_Task恢复存储后调用UpgradeStorage_Bind。它检查运行上下文、协议版本、304/288 B消息尺寸及可信keys，再绑定Stage；旧296/20 B消息不会被当作新版。
4. M1/M2/M3分别绑定真实UpgradeQuiesceParticipant，并在最终TX/配置提交/持久变更前检查UpgradeQuiesce_AllowsMutation。四个角色为Modbus、Storage、Config、Network；关闭M2构建时不要求Network角色。
5. 各关键模块登记Monitor_RegisterHealth，再按实际进展、有限等待和自检结果报告；同一源码的业务任务必须落实试运行门控。M4未代改队友调用点，任务创建或接口注册都不证明这些行为已完成。
6. storage_upgrade_tf提供UpgradeTfSource：open取得不可变文件租约、read完整值拷贝、close幂等且确认后台不再引用缓冲，所有I/O转交Storage_Task。不因插卡/启动自动导入。
7. M2将授权后的下载交给升级执行任务，依次调用UpgradeEntry_Begin/Write/Finish；MQTT回调只处理通知和调度，不同步执行Flash/密码运算。M1串口配置与Boot恢复使用各自入口，不通过App收包接口。

耗时操作只由升级执行任务调用，不在Storage_Task、Modbus、MQTT、网络核心线程或Monitor执行。M4运行时拒绝已登记的存储/业务/监督任务，并要求历史最小空闲栈至少4096 B；建议执行任务初配至少6 KiB普通SRAM并实测完整调用链。栈检查不代替传输侧选择正确任务，也不证明所有栈峰值。尚未新建这个执行任务或扩大其他人的栈。

## 成员接口核对（2026-11-10）

2026-11-10只读核对：M3 m3/storage-upgrade为422af956e4c8b533e7cb469c5ec267358ed06d8e，main为c9ceccd。四份升级源码与aa03dfb相同；时间服务等后续提交不等于升级契约已适配。M4已推送的基础提交为c670db3，持久版本统一和本轮存储端实现均为本地改动，未推送；没有合入整个M3分支。

装配已启用Storage_Init/Storage_Task；Config/MQTT/Telemetry仍保持NULL。下列M1/M2结论来自此前已核实提交，不推断成员未发布的新工作。当前构建资源见末节，任务禁用导致的链接回收不代表业务接通。

| 对接 | 实际已有接口/实现 | 现在能否直接接入升级 |
| --- | --- | --- |
| M1 Modbus | mb_inject_write、mb_take_slave_snapshot、mb_apply_slave_config等业务接口 | 业务可复用；尚无升级暂停、在途结算、恢复及最终TX门控接口，不能用任务挂起代替 |
| M1 配置/维护帧 | Config_Init/Config_Task仅声明，装配仍NULL；未发现共用maintenance_frame实现 | 不能接串口配置或正式Boot维护帧；需要M1提供真实实现 |
| M2 网络/上报 | MqttClient_Init/Task/Publish/IsConnected/ReportEvent已实现 | 可复用连接与发布能力；当前Publish为QoS0，不能当作已满足最终QoS1/结果可靠上报。network_upgrade仍只有.gitkeep，无升级通知授权、下载器及执行任务 |
| 存储升级队列 | 本地storage_upgrade_q和storage_upgrade已采用共同7类操作及版本2日志 | Task内真实绑定调用已接入；真实信任资料缺失时不会开放后端；启用任务不代表实板已运行 |
| TF文件服务 | 本地storage_upgrade_tf实现open/read/close租约，所有FatFs调用仍在Storage_Task | 已写入Task绑定调用，支持无卡时明确NOT_READY；尚未运行真实文件用例 |
| 暂停与健康 | M4已提供UpgradeQuiesce_Bind、Monitor_RegisterHealth/ReportProgress等 | 队友实际回调和报告尚未接入；注册成功或任务已创建不能替代运行证据 |
| M4查询 | UpgradeEntry_GetStatus、App_UpgradeGetBootIdentity已有实现 | 授权/上下文满足后可用于适配；不是现成的云端任务持久结果表，M2仍须关联任务与确认/回退结果 |

本地已复用现有SPI/SD驱动并补齐存储端，替代远程422af95中的V1预检、68 B日志及旧队列。没有把旧日志导入新版，也没有自动格式化UPGLOG。

storage_upgrade_init只登记介质和Storage_Task所有者；RESERVE_TX先检查保护视图，再使用shared高水位代码。BEGIN核对事务、预期序号及可写槽后取得租约、擦候选；失败保留租约供取消。CHUNK写入并读回成功才推进，首块1 B同样有效；失败后拒绝继续写。COMMIT_READY只接受本租约对应的READY/INSTALL，先写另一副本正文、再另页提交、最后Load核对。已尝试提交的请求不会重写，取消也须先证明仍是原完整基线；无法判定时保留占用。

Storage_Task每轮处理至多一个升级请求、一个TF请求，再让出1 tick；M2记录写队列也每轮至多一条，避免持续生产饿死取消。1 tick在当前1000 Hz配置下为1 ms，不代表真实I/O吞吐。控制日志恢复移入任务，未修改扫描算法或把历史卡死宣称为已修复。

SPI新增*_until接口，统一绝对期限覆盖取锁、命令和忙等待；普通调用方保持原API。TF只读调用期间在存储所有者内临时替换FatFs磁盘读回调，按同一期限单扇区轮询，返回前恢复原驱动，不改CubeMX生成文件或.ioc。FIL、队列和工作缓冲使用普通SRAM；没有DMA后台继续引用客户缓冲。

TF租约依赖当前_FS_LOCK=2（编译断言至少2），与记录文件各占一个文件锁；同一文件改写/删除由FatFs拒绝。插拔使租约失败，卸载后旧FIL才失效；close未明确成功时不复用客户端租约。租约期间其他临时文件打开可能因锁名额不足返回错误，不把它当作文件服务成功。所有队列为值拷贝，迟到响应按请求号和租约过滤。

主线任务选择、真实密钥、授权、各模块暂停/结算/同步及健康报告仍决定能否实际启用；本轮没有用注册成功代替这些行为，也未新增升级执行任务。

## 存储实际接入

Storage_Init只创建基础资源和初始化介质；Storage_Task恢复配置、序号、身份、时间下界、控制日志及TF只读视图，再分别上报资源和健康。全空、读失败、已提交损坏分别处理。序号初始化只读，高水位须高于已恢复的控制/记录序号；首次分配同样先提交完整预留窗口。

storage_runtime串行化完整业务操作，试运行或升级暂停时拒绝新变更；跨任务序号分配通过值拷贝代理转交Storage_Task。业务Flash和文件I/O使用固定操作期限，文件扇区同步轮询，返回前不遗留DMA引用。未启用MQTT、Telemetry或Config任务，存储端保持M3代码风格。

试运行只读检查TF现有卷、环状态、当前文件长度及最近记录格式/CRC，不建目录、段文件或写回。缺卡/无文件系统按未配置降级；读取失败和格式损坏不作为空白。自动写入自检默认关闭；正常缓存恢复也不因读取失败自动mkfs。

暂停先关闭准入，再等待在途完整操作退出；结算要求M2请求、配置事务、控制回收及TF租约均无待处理对象，否则BUSY并保留对象。同步由Storage_Task执行f_sync及环状态写入/读回。同步超时保留pending身份，后台未结束时恢复回调返回BUSY，不开放业务。

存储登记2 s进展期限、30 s单次等待上限，启动恢复预算20 s、业务轮次5 s。实际I/O完成才报告进展，不延长操作总期限；监督故障锁存后不恢复写入资格。控制回收每轮一个64 KiB块；暂停期间升级、取消和结算队列继续处理。M2队列已补齐静态消息区，旧CCM分配大小不变。

本轮AppDebug/Release构建、16个相关单元严格ARM诊断、13个单元静态分析通过。AppDebug：Flash303484/458752 B、普通SRAM107152/131008 B、CCM27096/65536 B；临时保留全部升级入口链接为311476/109568/27096 B。日志storage-runtime-*。未运行本机C模型、实板、吞吐或断电用例。

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
| Write | 1～1016 B输入复制到普通SRAM；适配拆成≤256 B。完整写后逐字节读回，成功才推进；相同重复块只读比较、不重写、不推进或续期 |
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

以下为源码路径审查范围，不是已执行的主机/实板用例：

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

第三轮只整理TF和Storage的读取重复逻辑：HashFile/CopyFile共用ReadBlock，保留每遍180000 ms固定起点，复制读取额外限制单次等待最多30000 ms。只在读取成功、未到期且未观察到取消时消费该块；读取期间收到取消也在返回后检查。文件完整填充约定及close/abort错误处理不变。

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

Initialize只由显式首次配置调用，必须全部[0,0x7000)升级元数据为FF，才写generation=1/last_allocated=0；不擦除任何残留。启动、BEGIN和恢复不自动初始化；已有事务却丢失高水位不能归零。Boot每次分配/首次配置固定10000 ms总期限。物理维护UART命令GWRTINIT只用于显式首次配置，非空元数据不得借此归零。

已存在可信事务的续装/回退不需分配新ID，新恢复需要有效分配记录。高水位丢失须从可信留档维修，不能自动重置；不复用保证以介质未被人为回滚为前提，CRC不是安全单调硬件。首次实际配置和新基线写板需另有明确区域/内容授权，本轮未执行。

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

App从入口起最长30 s，可信平台/产品自检连续健康1 s才请求确认，不要求外部网络在线或TF存在。Monitor聚合各关键模块登记/真实进展/自检，合法等待不能续期，过期或致命平台故障锁存停止喂狗；确认前再次原子检查，避免Poll后故障仍写确认。缺实际报告不自动确认，M1/M2/M3试运行业务门控仍需接入，不能称整机试运行已验收。

硬件IWDG当前仍/256、4095，按LSI范围约22.31～61.68 s，不能视为满足拟定≤8 s目标；最终参数、Flash停顿和最长合法不可喂狗段须实板核算。软件固定期限不等同实板墙钟保证。

## M4/M3队列契约

以下共同契约已在本地storage_upgrade、storage_upgrade_q及storage_upgrade_tf中实现。远程M3 422af95/v0.9仍是旧296/20 B和68 B日志，不能再合入旧实现覆盖本地文件。当前软件编译与静态检查通过，尚无真实队列、文件或掉电联调证据。

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

ARM EABI下请求304 B/data@44、响应288 B/data@32，头文件已加静态断言；每组比旧296/20 B增加276 B，队列区和处理栈预算都要核对。未用字段清零、无指针；消息只供CPU时可按已审查布局放置，DMA实际缓冲须普通SRAM。深度1不自动保证单请求在途，M4服务仍串行修改。

result固定0 OK、1 INVALID、2 BUSY、3 IO_ERROR、4 TIMEOUT、5 CORRUPT、6 UNSUPPORTED、7 CONFLICT、8 NOT_READY，禁止直接强转HAL或日志结果枚举，HAL诊断存detail。expected_*_result使用UpgradeStateResult的0～5值，不是队列结果的0～8值。M4严格匹配req_id/type/session/tx_id，BEGIN新session和RESERVE_TX新ID为成功返回值例外；READ成功长度必须恰等请求，失败不使用部分数据，丢弃迟到应答不续期。

UpgradeStorage_GetDiagnostic只读返回当前/最近请求号、类型、偏移、槽、事务、映射结果及匹配应答的原始状态/detail/实际长度；不把旧应答detail冒充本次失败。它不访问队列、不更新进展，也不包含包内容、明文或密钥。

Storage_Task须有界唤醒/连续服务并保留控制/采样公平性，不能每个256 B请求固定延时1000 ms。最大包每遍至少1793次读取，固定每次1 s已超过180 s认证预算。吞吐需实板测量，不能只增加设备超时掩盖调度问题。

## 信任资料、工具与启用

UpgradeTrust_ReadPlatform默认返回false；可信平台实现只读保护区，提供运行期间不变的65 B SEC1公钥和32 B AES指针，当前批准密钥编号均为1。GATEWAY_UPGRADE_TRUST_SOURCE可显式加入该实现；不能把生产密钥、私钥或凭据写进通用源码、日志、TF或导出。Boot/App都先初始化同一映射，未知编号拒绝；没有真实资料时保持未配置。

tools/package_upgrade.py读取外部受控P-256私钥及设备AES文件，生成V2加密包，持久号默认且只允许2，明确固件版本并确认实际App接管IWDG。当前硬件标识仍为源码批准的F407-TEST；此标识不表示使用测试信任根。输入必须为0x08010000偏移且向量有效的App，不写出明文/密钥副本，输出前真实验签及GCM自检。PEM密码可从指定环境变量读取，不把密码放命令行。

nonce由每AES密钥唯一的SQLite账本预留，先持久递增再加密，失败消耗的值不复用；同账本并发受数据库事务串行。首次登记才用--initialize-ledger，不覆盖已有密钥记录；正常制作不自动新建丢失账本。每key只能有一个持续维护的账本，不能复制后并行用；丢失或回滚须轮换AES密钥，不能重置同key账本继续发行。该保证依赖可信发布环境及账本可靠保管。

tools/boot_recovery.py及既有本地发送工具的--persistence-version默认且只允许2；发送前核对包和设备INFO，要求明确compat=2，缺字段或其他值拒绝。--initialize-transactions仅在INFO显示首次空白时发送GWRTINIT；设备还完整检查全部元数据FF，失败不擦除重试。INFO增加兼容号、普通阶段/序号、事务/次数及高水位状态，保留原字段。

源码准备完成后，启用仍需以下外部交付：

1. 存储端、共同队列及TF租约已写入；Storage及存储侧门控/健康已接入，实际信任资料下的绑定/运行仍需实板核对；历史控制日志卡死不能仅凭编译关闭。
2. M2提供真实任务授权、网络下载、升级执行任务及最终结果上报；M1提供串口配置/共用维护帧。各模块接试运行/最终变更门控、真实暂停/结算/同步和健康报告；TF租约的真实介质行为仍需验证。
3. 工厂/维护流程提供真实保护区映射、已接管IWDG的初始App及匹配恢复包；不自动重生成测试信任根或擦旧日志。Boot/App/M3/工具均采用版本2，普通升级不更新Boot、不隐式迁移持久格式。
4. 集中实板验证真实存储/吞吐、取消/关闭、断电窗口、两次试运行、错误确认拒绝、看门狗交接/停止及栈堆峰值，最后核对正式构建和测试根隔离。

当前Boot电脑工具仍使用GWRC等工程恢复协议，尚未接正式设计的A5 5A维护帧/0x30～0x32。共用帧编解码由M1提供，Boot侧正式封装、统一工具接入和联调仍需完成；现有串口安全机制及工具检查不能冒充该协议验收。

## 已有验证及限制

上轮存储端源码适配：AppDebug/Release构建通过，7个相关单元严格ARM编译、5个单元-fanalyzer通过。为排除默认任务禁用导致的链接回收，临时强制保留Storage_Init/Storage_Task及网络/TF升级入口，实际链接并核对核心/驱动符号；Flash 311084/458752 B、普通SRAM 109104/131008 B、CCM 27096/65536 B。正式选项未改，临时ELF/map清理；数字不含未来网络下载器、新执行任务栈或动态峰值。日志为.local/tests/logs/m3-upgrade-*。

本轮错误路径采用源码审查和ARM静态分析，未运行C模型或实板用例；覆盖到期请求不I/O、源槽保护、预留失败、BEGIN/写入失败保留租约、错事务取消、提交不明不重写、文件短读/拔卡/关闭失败及迟到应答。下表为启用Storage前的历史构建，当前资源以“存储实际接入”一节为准。

2026-11-10在c670db3后统一版本2，7个标准预设均重新配置并ARM编译/链接通过，日志.local/tests/logs/v2-unify-*-20261110.log。

| 配置 | Flash / 容量 | 普通SRAM / 容量 | CCM / 容量 |
| --- | --- | --- | --- |
| AppDebug | 161420 / 458752 B | 91672 / 131008 B | 11264 / 65536 B |
| Debug | 161420 / 524288 B | 91672 / 131072 B | 11264 / 65536 B |
| Release | 103288 / 524288 B | 91656 / 131072 B | 11264 / 65536 B |
| BootDebug | 57748 / 65536 B | 18216 / 131008 B | 0 / 65536 B |
| BootSizeCheck | 54928 / 65536 B | 18216 / 131008 B | 0 / 65536 B |
| BootReceiveTest只读诊断 | 39056 / 65536 B | 18160 / 131008 B | 0 / 65536 B |
| BootRecoveryTest | 57812 / 65536 B | 18216 / 131008 B | 0 / 65536 B |

Debug/Release是原址开发构建，不能代替0x08010000偏移的升级App。上述App仍有任务未绑定，链接器可回收尚未调用的入口，数字不是完整业务或TLS预算。

既有纯Python恢复工具测试57项通过，包含缺失compat及compat=1拒绝，使用假串口。版本统一前的严格编译、静态分析和接口强制保留链接证据保存在.local/tests/logs，历史双基线资源数字不作为当前结果。版本统一时另有24个相关单元严格ARM诊断（-Wextra -Werror -Wshadow）通过；CMake、头文件及工具均拒绝版本1，打包器默认2的真实签名/GCM自检及临时账本初始化通过。日志v2-unify-strict/tools-20261110.log。

常规App构建仍有LwIP调试格式及M1 modbus_test既有警告（含Release的测试字符串越界警告），本轮未代改；受检M4单元及Boot构建没有新增警告。

未执行受Windows应用控制阻止的本机C模型，未下载、访问串口或擦写Flash，未把构建及工具模拟当作并发、密码耗时、掉电恢复或整机验收。本轮按用户授权修改存储升级服务及必要调度/驱动，M1/M2业务实现未改；真实任务及依赖尚未全部启用，NOT_READY仍是默认边界。
