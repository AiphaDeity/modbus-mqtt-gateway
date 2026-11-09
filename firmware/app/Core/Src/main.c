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
#include "cmsis_os.h"
#include "dma.h"
#include "eth.h"
#include "fatfs.h"
#include "i2c.h"
#include "sdio.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include "ethernet.h"   /* Day2：m2_ethernet_init() 声明（bsp/ethernet/ethernet.h） */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

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

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("boot: USART1 OK, before DMA\r\n");
  MX_DMA_Init();
  printf("boot: DMA OK, before ETH\r\n");
  MX_ETH_Init();
  printf("boot: ETH OK\r\n");
  MX_I2C1_Init();
  printf("boot: I2C1 OK\r\n");
  MX_SPI2_Init();
  printf("boot: SPI2 OK\r\n");
  MX_USART2_UART_Init();
  MX_USART3_UART_Init();
  MX_TIM7_Init();
  MX_SDIO_SD_Init();
  printf("boot: SDIO OK\r\n");
  MX_FATFS_Init();
  printf("boot: FATFS OK\r\n");
  printf("[DBG] all MX_*_Init done\r\n"); fflush(stdout);

  /* ★排障：打印本次复位原因（掉电/外部复位/软件/看门狗），打印后清标志 */
  {
    uint32_t csr = RCC->CSR;
    printf("[BOOT] reset: POR=%d BOR=%d PIN=%d SW=%d IWDG=%d WWDG=%d LPWR=%d CSR=0x%08lX\r\n",
           (csr & RCC_CSR_PORRSTF)  ? 1 : 0,
           (csr & RCC_CSR_BORRSTF)  ? 1 : 0,
           (csr & RCC_CSR_PINRSTF)  ? 1 : 0,
           (csr & RCC_CSR_SFTRSTF)  ? 1 : 0,
           (csr & RCC_CSR_IWDGRSTF) ? 1 : 0,
           (csr & RCC_CSR_WWDGRSTF) ? 1 : 0,
           (csr & RCC_CSR_LPWRRSTF) ? 1 : 0,
           (unsigned long)csr);
    fflush(stdout);
    __HAL_RCC_CLEAR_RESET_FLAGS();
  }
  /* USER CODE BEGIN 2 */
  printf("Gateway boot\r\n");
  /* ★排障修复：osKernelInitialize 必须先于任何 os 对象创建（MX_FREERTOS_Init 会检查内核状态）。 */
  osKernelInitialize();
  /* ★★核心修复：网卡初始化【不能】放在这里！
     m2_ethernet_init() 内部会调 tcpip_init / netif_add / sys_thread_new，
     其中 ethernet.c 用 taskENTER_CRITICAL()/taskEXIT_CRITICAL() 包着建线程。
     调度器启动前 port.c 的 uxCriticalNesting 初始值是哨兵值 0xAAAAAAAA，
     一次 enter/exit 配平后它仍 ≠ 0 → BASEPRI 永远停在 0x50 不再恢复 →
       ① 所有优先级 ≥5 的中断被屏蔽（TIM6(优先级15,实测IPR=0xF0) 也不再进中断 →
          uwTick 停在 10，HAL_ETH_Start_IT 里的 HAL_Delay(1) 死循环）
       ② ETH 接收中断(优先级5) 被屏蔽 → 收包线程永远收不到包 → PING 不可能通
     所以网卡初始化移到了任务上下文（integration/app_entry.c 的 App_Entry 里）。 */
  /* USER CODE END 2 */

  /* Init scheduler */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  Error_Handler();
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
  RCC_OscInitStruct.PLL.PLLQ = 7;
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
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
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
