/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
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
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum
{
  Short = 0,
  Long  = 1
} Msg_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BTN_PRESSED_LEVEL  GPIO_PIN_RESET   // active-low (board has external 100K pull-up, R23)
#define LONG_PRESS_MS      1000
#define DEBOUNCE_MS        150
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
osMessageQueueId_t Task1QueueHandle;

osThreadId_t BtnDebounceTaskHandle;
const osThreadAttr_t BtnDebounceTask_attributes = {
  .name = "BtnDebounceTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

static volatile uint32_t pressTick      = 0;
static volatile uint8_t  isPressed      = 0;
static volatile uint8_t  debounceLocked = 0;   // 1 = 正在等待前一次邊緣的確認結果
/* USER CODE END Variables */
/* Definitions for Task02 */
osThreadId_t Task02Handle;
const osThreadAttr_t Task02_attributes = {
  .name = "Task02",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for Task01 */
osThreadId_t Task01Handle;
const osThreadAttr_t Task01_attributes = {
  .name = "Task01",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for PeriodicTimer */
osTimerId_t PeriodicTimerHandle;
const osTimerAttr_t PeriodicTimer_attributes = {
  .name = "PeriodicTimer"
};
/* Definitions for myMutex01 */
osMutexId_t myMutex01Handle;
const osMutexAttr_t myMutex01_attributes = {
  .name = "myMutex01"
};
/* Definitions for Task1Sem */
osSemaphoreId_t Task1SemHandle;
const osSemaphoreAttr_t Task1Sem_attributes = {
  .name = "Task1Sem"
};
/* Definitions for Task2Sem */
osSemaphoreId_t Task2SemHandle;
const osSemaphoreAttr_t Task2Sem_attributes = {
  .name = "Task2Sem"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void LED_Blink(int, int);
void Task_BtnDebounce(void *argument);
/* USER CODE END FunctionPrototypes */

void Task_2(void *argument);
void Task_1(void *argument);
void Callback01(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Create the mutex(es) */
  /* creation of myMutex01 */
  myMutex01Handle = osMutexNew(&myMutex01_attributes);

  /* USER CODE BEGIN RTOS_MUTEX */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of Task1Sem */
  Task1SemHandle = osSemaphoreNew(1, 0, &Task1Sem_attributes);

  /* creation of Task2Sem */
  Task2SemHandle = osSemaphoreNew(1, 0, &Task2Sem_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* creation of PeriodicTimer */
  PeriodicTimerHandle = osTimerNew(Callback01, osTimerPeriodic, NULL, &PeriodicTimer_attributes);

  /* USER CODE BEGIN RTOS_TIMERS */
  osTimerStart(PeriodicTimerHandle, 10000);   // 每 10 秒觸發一次 Callback01
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  Task1QueueHandle = osMessageQueueNew(4, sizeof(uint8_t), NULL);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of Task02 */
  Task02Handle = osThreadNew(Task_2, NULL, &Task02_attributes);

  /* creation of Task01 */
  Task01Handle = osThreadNew(Task_1, NULL, &Task01_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  BtnDebounceTaskHandle = osThreadNew(Task_BtnDebounce, NULL, &BtnDebounceTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* USER CODE END RTOS_EVENTS */
}

/* USER CODE BEGIN Header_Task_2 */
/**
  * @brief  Function implementing the Task02 thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_Task_2 */
void Task_2(void *argument)
{
  /* USER CODE BEGIN Task_2 */
  for (;;)
  {
    osSemaphoreAcquire(Task2SemHandle, osWaitForever);   // 等 Timer 通知
    osMutexAcquire(myMutex01Handle, osWaitForever);      // 取得 LED2 使用權
    LED_Blink(10, 2000);                                  // 10Hz，持續 2 秒
    osMutexRelease(myMutex01Handle);
  }
  /* USER CODE END Task_2 */
}

/* USER CODE BEGIN Header_Task_1 */
/**
  * @brief  Function implementing the Task01 thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_Task_1 */
void Task_1(void *argument)
{
  /* USER CODE BEGIN Task_1 */
  uint8_t msg;
  for (;;)
  {
    if (osMessageQueueGet(Task1QueueHandle, &msg, NULL, osWaitForever) == osOK)
    {
      osMutexAcquire(myMutex01Handle, osWaitForever);
      if (msg == Long)
        LED_Blink(10, 5000);   // 長按：10Hz，持續 5 秒
      else
        LED_Blink(1, 5000);    // 短按： 1Hz，持續 5 秒
      osMutexRelease(myMutex01Handle);
    }
  }
  /* USER CODE END Task_1 */
}

/* USER CODE BEGIN Header_Task_BtnDebounce */
/**
  * @brief  等待按鈕邊緣通知，用 osDelay 讓彈跳穩定後再確認狀態，
  *         判斷長按/短按後把結果送進 Task1Queue。
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_Task_BtnDebounce */
void Task_BtnDebounce(void *argument)
{
  /* USER CODE BEGIN Task_BtnDebounce */
  for (;;)
  {
    osSemaphoreAcquire(Task1SemHandle, osWaitForever);   // 等 ISR 通知有邊緣發生
    osDelay(DEBOUNCE_MS);                                 // Task context，安全地等彈跳穩定

    uint32_t now   = osKernelGetTickCount();
    uint8_t  state = HAL_GPIO_ReadPin(UserButton_GPIO_Port, UserButton_Pin);

    if (state == BTN_PRESSED_LEVEL)
    {
      pressTick = now;
      isPressed = 1;
    }
    else if (isPressed)
    {
      isPressed = 0;
      uint8_t msg = ((now - pressTick) >= LONG_PRESS_MS) ? Long : Short;
      osMessageQueuePut(Task1QueueHandle, &msg, 0, 0);
    }

    debounceLocked = 0;   // 這次確認流程結束，才開放接受下一次邊緣
  }
  /* USER CODE END Task_BtnDebounce */
}

/* Callback01 function */
void Callback01(void *argument)
{
  /* USER CODE BEGIN Callback01 */
  osSemaphoreRelease(Task2SemHandle);
  /* USER CODE END Callback01 */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void LED_Blink(int f, int duration_ms)
{
  int half_period = 1000 / (2 * f);
  int count = duration_ms / half_period;
  for (int i = 0; i < count; i++)
  {
    HAL_GPIO_TogglePin(GPIOB, LED2_Pin);
    osDelay(half_period);
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin != UserButton_Pin)
    return;

  if (debounceLocked)
    return;                       // 還在等前一次確認結果，忽略這次彈跳雜訊

  debounceLocked = 1;
  osSemaphoreRelease(Task1SemHandle);   // 通知 Task_BtnDebounce 去確認
}
/* USER CODE END Application */
