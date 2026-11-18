/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "spi.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "boot_watchdog.h"
#include <stdio.h>
#include "boot_upgrade_state.h"
#include "boot_trial.h"
#include "boot_recovery.h"
#include "boot_install.h"
#include "upgrade_trust.h"
#include "boot_jump.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
// 启狗后到 Flash 唤醒检查点的准备期限，单位 ms；安装和收包使用各自的期限。
#define BOOT_STARTUP_TIMEOUT_MS 15000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/*
 * 长操作返回后检查 Boot 已锁存的监督失败，stage 仅标明阶段；失败时主动软件复位。
 * 若代码一直阻塞而无法回到这里，就没有机会执行这条软件复位路径，由未获刷新的 IWDG 兜底。
 */
static void Boot_ResetIfWatchdogFailed(const char *stage)
{
  if (Boot_WatchdogFailed())
  {
    printf("WDG supervision expired stage=%s; reset\r\n", stage);
    __DSB();
    NVIC_SystemReset();
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */


  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  // 1. 先保存复位原因和 App 留下的确认意图，稍后交给安装状态机判断。
  uint32_t reset_flags = RCC->CSR;
  // 只有软件复位标志存在，且没有同时记录掉电、欠压或看门狗复位，才允许采信确认邮箱。
  bool software_reset = ((reset_flags & RCC_CSR_SFTRSTF) != 0U)
                     && ((reset_flags & (RCC_CSR_PORRSTF | RCC_CSR_BORRSTF
                                        | RCC_CSR_IWDGRSTF | RCC_CSR_WWDGRSTF)) == 0U);
  // 保存可能有效的确认消息并清除旧邮箱；此处尚不决定确认或回退。
  Boot_TrialBegin((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS, software_reset);
  // 原因已保存，清除硬件标志，避免下次复位与这次混在一起判断。
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */



  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_SPI3_Init();
  MX_USART2_UART_Init();
  /* USER CODE BEGIN 2 */

  // 2. 时钟配置完成，初始化 GPIO、日志串口、外部 Flash 的 SPI3 和 USART2 维护口。

  // 基础外设就绪后启动 IWDG，后面的 Flash 准备、安装和维护都接受监督。
  // 有界操作各自保存起点，只在实际进展后刷新；本次 Boot 一旦锁存故障就不再恢复喂狗。
  if (!Boot_WatchdogStart())
  {
    Error_Handler();
  }
  // 保存本段准备流程的起点，后面的期限检查使用同一个起点，不随日志或进展重置。
  uint32_t boot_started_ms = HAL_GetTick();
  printf("Boot watchdog active\r\n");

  // 向外部 Flash 发送退出深度掉电命令：拉低片选、发送 0xAB、拉高片选结束。
  uint8_t wake_command = 0xABU;
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_RESET);
  HAL_StatusTypeDef wake_status = HAL_SPI_Transmit(&hspi3, &wake_command, 1U, 10U);
  HAL_GPIO_WritePin(EXT_SPI_FLASH_CS_GPIO_Port, EXT_SPI_FLASH_CS_Pin, GPIO_PIN_SET);
  HAL_Delay(1U);
  // 这里只记录命令传输状态，Flash 内容是否可用还须由后续读取和认证判断。
  printf("Flash wake: HAL status=%u\r\n", (unsigned int)wake_status);

  // 本段准备未超期才报告进展并喂狗；已超期则锁存失败并复位。
  if (!Boot_WatchdogProgress(boot_started_ms, BOOT_STARTUP_TIMEOUT_MS))
  {
    Boot_ResetIfWatchdogFailed("startup");
    Error_Handler();
  }

  // 3. 取得可信资料，再调用安装状态机处理现有持久状态。
  // 信任资料来自 Boot 预留区，初始化失败时下面的取密钥操作会拒绝放行。
  (void)UpgradeTrust_Initialize();
  const uint8_t *public_key = NULL;     // 65 B 验签公钥的只读指针。
  const uint8_t *aes_key = NULL;        // 32 B AES 解密密钥的只读指针。
  // 查询当前支持的密钥编号 1/1；成功后两个指针才可用于包认证。
  bool keys_ready = UpgradeTrust_Get(1U, 1U, &public_key, &aes_key);

  // keys_ready 为 true 才调用 Boot_InstallRun；否则直接得到状态错误，转入维护处理。
  // 此调用按日志决定正常启动、安装候选、处理确认或恢复旧包；正常上电不一定需要擦写 App。
  Boot_InstallResult install_result = keys_ready ? Boot_InstallRun(public_key, aes_key) : BOOT_INSTALL_STATE_ERROR;

  // 先处理安装期间的监督故障，再用 install_result 判断是否允许继续启动。
  Boot_ResetIfWatchdogFailed("install");
  printf("Install result=%u\r\n", (unsigned int)install_result);

  // 已确认 App 可启动、候选可试运行、旧包恢复成功，这三类结果才允许继续交接检查。
  bool allow_app = (install_result == BOOT_INSTALL_BOOT_EXISTING)
                || (install_result == BOOT_INSTALL_TRIAL_STARTED)
                || (install_result == BOOT_INSTALL_RESTORED);
  // 结果允许还不够，App 的初始栈指针和复位入口也须有效。
  allow_app = allow_app && Boot_CheckAppVector();
  // 不能启动时必须驻留维护；若本来可启动，仍先留一个维护窗口给电脑接入。
  bool serial_service = !allow_app;

  // 4. 按上一步的结果选择直接交接试运行，或进入 USART2 维护窗口。
  // TRIAL_STARTED 已记录本次放行机会，跳过窗口；其他情况进入循环查询或恢复。
  // 同一次启动不能无故再次运行安装器，否则可能被当作未确认的下一次试运行。
  while ((install_result != BOOT_INSTALL_TRIAL_STARTED) || !allow_app)
  {
    // 返回 PACKAGE 才表示完整恢复包已认证并提交 READY，主流程才能再调用安装器。
    // COMMAND 表示继续维护处理，NO_REQUEST 表示本轮窗口未接到请求。
    BootReceiveResult received = Boot_RecoveryReceive(public_key, aes_key);
    Boot_ResetIfWatchdogFailed("receive");
    if (received == BOOT_RECEIVE_COMMAND)
    {
      // 维护命令、未完成接收或串口错误等都可能走这里，不能当作已有完整包。
      // 一旦驻留维护，无请求时也继续等待；电脑可完成恢复，或用 RUN 请求复位重判。
      serial_service = true;
      continue;
    }
    if (received == BOOT_RECEIVE_NO_REQUEST)
    {
      // App 原本可启动且尚未驻留维护：窗口结束后退出循环，继续最终交接。
      // App 不可启动或已驻留维护：保留 Boot 服务，等待电脑下一条请求。
      if (allow_app && !serial_service)
      {
        break;
      }
      HAL_Delay(10U);
      continue;
    }

    // 走到这里表示收到 PACKAGE：恢复 READY 已落盘，现在再次调用安装状态机安装恢复包。
    // 与开机首遍一样，缺少可信密钥就返回状态错误，不执行安装器。
    install_result = keys_ready ? Boot_InstallRun(public_key, aes_key) : BOOT_INSTALL_STATE_ERROR;
    Boot_ResetIfWatchdogFailed("install");
    // 向电脑回复本次安装结果；TRIAL 仅表示已登记试运行，后续仍需 App 健康确认。
    // 这是同一 FINISH 的第二条阶段应答，第一条 ACCEPTED/READY 已在收包入口发出。
    Boot_RecoveryReplyResult(install_result);
    // 重新核对安装结果和 App 向量，通过才退出维护循环，进入交接步骤。
    allow_app = ((install_result == BOOT_INSTALL_BOOT_EXISTING)
                 || (install_result == BOOT_INSTALL_TRIAL_STARTED)
                 || (install_result == BOOT_INSTALL_RESTORED)) && Boot_CheckAppVector();
    if (allow_app)
    {
      break;
    }
    // 本次仍不能启动，继续驻留维护，等待电脑显式恢复。
    serial_service = true;
  }
  // 保护性检查：没有取得启动资格就停在 Boot，不能继续执行下面的跳转。
  if (!allow_app)
  {
    printf("Upgrade requires install/restore/recovery; stay in Boot\r\n");
    while (1)
    {
      HAL_Delay(1000U);
    }
  }

  // 5. 最终交接检查：复查向量、写启动身份，再认证最终启动包并核对片内 App。
  if (Boot_CheckAppVector())
  {
    // 这里写给下一次 App 启动读取的 NORMAL/TRIAL 身份，与步骤 1 读取的确认消息方向相反。
    if (!Boot_TrialPrepareApp((volatile uint32_t *)UPGRADE_MAILBOX_ADDRESS))
    {
      Error_Handler();
    }
    printf("App vector valid\r\n");
    // 根据最终持久状态选择包，核对真实镜像及 IWDG 交接能力；失败进入错误处理。
    if (!Boot_WatchdogPrepareApp(public_key, aes_key))
    {
      Boot_ResetIfWatchdogFailed("handoff");
      Error_Handler();
    }
    // 6. 清理硬件和中断、设置 App 向量表、切换 MSP，再跳到 App Reset_Handler。
    // 成功后不会返回 main；IWDG 持续计数，由 App 接管。
    if(!Boot_JumpToApp())
    {
      Error_Handler();
    }
  }
  else
  {
    printf("App vector invalid\r\n");
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  // 正常启动已在上方交接；若最终向量检查失败来到这里，则不喂狗，由已启动的 IWDG 复位。
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
// 将 printf 的单个字符送往 USART1，发送失败返回 EOF。
int __io_putchar(int ch)
{
  uint8_t data = (uint8_t)ch;
  if (HAL_UART_Transmit(&huart1, &data, 1U, 10U) != HAL_OK)
  {
    return EOF;
  }
  return (int)data;
}
/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  // 关闭中断并停止执行后续流程；若 IWDG 已启动，停止喂狗后会超时复位。
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
