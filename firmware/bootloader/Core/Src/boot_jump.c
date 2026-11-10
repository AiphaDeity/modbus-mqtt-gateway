#include "boot_jump.h"
#include "main.h"
#include "usart.h"

// 普通SRAM首64B为复位邮箱；栈指针须位于其后，允许等于SRAM末端后一地址。
#define BOOT_SRAM_START_ADDRESS 0x20000040U
#define BOOT_SRAM_END_ADDRESS   0x20020000U
// 候选应用 Flash 区间：[0x08010000, 0x08080000)，起点同时为向量表基址
#define BOOT_APP_START_ADDRESS  0x08010000U
#define BOOT_APP_END_ADDRESS    0x08080000U

/*
 * 关闭串口并恢复复位时钟配置，须在 HAL 滴答仍运行时调用。
 * 失败可能已关闭部分硬件，不恢复外设，也不保证串口仍可输出。
 */
static bool Boot_DeinitHardware(void)
{
  // 禁用 USART1，并通过已有 MSP 函数关闭串口时钟、释放 PA9/PA10
  if(HAL_UART_DeInit(&huart1) != HAL_OK)
  {
    return false;
  }

  // 将系统时钟切回 HSI，并关闭 HSE、PLL 等时钟配置
  if(HAL_RCC_DeInit() != HAL_OK)
  {
    return false;
  }

  return true;
}

/*
 * 硬件清理后停止 SysTick，关闭外设中断并清除挂起状态。
 * 返回时中断仍屏蔽，禁止再等待 HAL 滴答；最终交接时恢复中断。
 */
static void Boot_StopInterrupts(void)
{
  // 暂时屏蔽可屏蔽中断，避免清理过程中进入处理函数
  __disable_irq(); 
  
  // 停止 Bootloader 的系统滴答并清理计数状态
  SysTick->CTRL = 0U;
  SysTick->LOAD = 0U;
  SysTick->VAL = 0U;

  // ICTR 给出中断组数减一；每组对应 32 个中断的使能和挂起位
  uint32_t group_count = (SCnSCB->ICTR & SCnSCB_ICTR_INTLINESNUM_Msk) + 1U;
  // ICER、ICPR 均通过写 1 清除对应状态，分别关闭中断和清除挂起请求
  for (uint32_t i = 0U; i < group_count; i++)
  {
  NVIC->ICER[i] = 0xFFFFFFFFU;
  NVIC->ICPR[i] = 0xFFFFFFFFU;
  }

  // SysTick 和 PendSV 属于系统异常，需通过 ICSR 单独清除挂起状态
  SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

  // 等待寄存器访问完成，并同步后续指令执行
  __DSB();
  __ISB();
}

/*
 * 从裸机 Bootloader 的特权线程上下文交接到应用，成功后不返回。
 * initial_sp 为已检查的初始 MSP，reset_vector 为保留 Thumb 标记的复位入口。
 * 调用前须完成硬件和中断清理、设置 VTOR 并执行内存屏障；
 * BASEPRI、FAULTMASK 须保持复位值 0，应用复位入口不得返回。
 * 参数通过 r0、r1 传入；切换 MSP 后只执行汇编，不再访问旧函数栈。
 */
__attribute__((naked, noreturn))
static void Boot_EnterApp(uint32_t initial_sp, uint32_t reset_vector)
{
  __asm volatile
  (
    "movs r2, #0\n"
    "msr control, r2\n"
    "isb\n"
    "msr msp, r0\n"
    "cpsie i\n"
    "bx r1\n"
  );
}

// 检查 MSP 范围/对齐及 Thumb 入口地址，不检查固件认证或完整性。
bool Boot_IsAppVectorValid(uint32_t initial_sp, uint32_t reset_vector)
{
  // 栈向低地址增长，初始 MSP 允许等于 SRAM 末端
  if ((initial_sp <= BOOT_SRAM_START_ADDRESS) ||
      (initial_sp > BOOT_SRAM_END_ADDRESS))
  {
    return false;
  }

  // 初始栈指针必须满足 8 字节对齐
  if ((initial_sp & 7U) != 0U)
  {
    return false;
  }

  // 复位向量最低位必须为 1，表示以 Thumb 状态执行入口代码
  if ((reset_vector & 1U) == 0U)
  {
    return false;
  }

  // 清除 Thumb 标记后检查指令地址，保留原始入口值
  uint32_t entry_address = reset_vector & ~1U;
  if ((entry_address < BOOT_APP_START_ADDRESS) ||
      (entry_address >= BOOT_APP_END_ADDRESS))
  {
    return false;
  }

  return true;
}


// 从固定 App 基址读取 MSP 和复位向量，仅返回地址约束检查结果。
bool Boot_CheckAppVector(void)
{
  uint32_t initial_sp = *(volatile const uint32_t *)BOOT_APP_START_ADDRESS;
  uint32_t reset_vector = *(volatile const uint32_t *)(BOOT_APP_START_ADDRESS + 4U);
  return Boot_IsAppVectorValid(initial_sp, reset_vector);
}


/*
 * 清理硬件后从特权线程跳转 App，成功不返回；须先完成包认证及最终放行检查。
 * 调用时中断和 HAL 滴答正常，BASEPRI/FAULTMASK 为 0，镜像在交接期间不可改写。
 * 向量或清理失败返回 false，此时部分硬件可能已关闭。
 */
bool Boot_JumpToApp(void)
{
  // 保存最终交接所需的初始 MSP 和带 Thumb 标记的复位入口
  uint32_t initial_sp = *(volatile const uint32_t *)BOOT_APP_START_ADDRESS;
  uint32_t reset_vector = *(volatile const uint32_t *)(BOOT_APP_START_ADDRESS + 4U);

  if (!Boot_IsAppVectorValid(initial_sp, reset_vector))
  {
    return false;
  }

  // 硬件清理可能依赖 HAL 滴答，必须在停止中断和 SysTick 之前执行
  if(!Boot_DeinitHardware())
  {
    return false;
  }

  Boot_StopInterrupts();

  // VTOR 指向应用的整张向量表，不是 Reset_Handler 的入口地址
  SCB->VTOR = BOOT_APP_START_ADDRESS;

  __DSB();
  __ISB();

  // 同步向量表设置后进行最终交接，内部切换 MSP 并跳入 Reset_Handler
  Boot_EnterApp(initial_sp, reset_vector);
}
