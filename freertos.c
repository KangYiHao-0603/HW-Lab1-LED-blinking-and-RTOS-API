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
/*
 * ============================== 系統架構總覽 ==============================
 *
 *  (1) 定時閃燈
 *      [PeriodicTimer 每 10 秒] --Release Task2Sem--> [Task_2]
 *                                                        |
 *                                                   取得 mutex
 *                                                        v
 *                                                LED2 以 10Hz 閃 2 秒
 *
 *  (2) 按鈕長按 / 短按
 *      [按鈕 EXTI ISR] --Release Task1Sem--> [Task_BtnDebounce]
 *                                                  | 消抖 + 計算按住時間
 *                                                  v
 *                                     Task1Queue 放入 Short 或 Long
 *                                                  |
 *                                                  v
 *                                             [Task_1] --取得 mutex-->
 *                                                  Short : LED2 1Hz  閃 5 秒
 *                                                  Long  : LED2 10Hz 閃 5 秒
 *
 *  三種同步物件各自的用途：
 *
 *  Semaphore（號誌）→「通知有事情發生」
 *    - 本檔用 osSemaphoreNew(1, 0, ...) 建立：最大值 1、初始值 0（Binary Semaphore）。
 *    - Release：count 0 → 1（發出通知）。
 *    - Acquire：count 1 → 0 並繼續執行；若 count 已是 0，Task 會進入 Blocked 狀態等待，
 *               等待期間完全不佔用 CPU（跟 while 輪詢旗標不同）。
 *    - 為什麼需要：ISR 和 Timer callback 都不能做耗時或會阻塞的動作（不能 osDelay、不能等 mutex），
 *      所以它們只負責「Release 一下」，真正的工作交給在 Acquire 等待的 Task 去做。
 *
 *  Queue（訊息佇列）→「通知 + 傳資料」
 *    - Semaphore 只能告訴對方「發生了」，無法告訴對方「發生了什麼」。
 *    - 按鈕需要把「長按還是短按」這個資訊傳給 Task_1，所以用 queue 傳一個 Msg_t（Short/Long）。
 *    - Queue 還有緩衝功能：Task_1 閃燈 5 秒期間若又按了按鈕，結果會先排在 queue 裡（最多 4 筆），
 *      不會遺失，Task_1 做完再依序取出。
 *
 *  Mutex（互斥鎖）→「保護共享資源」
 *    - LED2 是 Task_1 和 Task_2 共用的硬體資源。
 *    - 若兩個 Task 同時在 toggle LED2，兩邊的 toggle 會交錯，閃爍頻率會亂掉。
 *    - 用 mutex 保證同一時間只有一個 Task 在使用 LED2，另一個要等對方 Release 才能開始。
 *    - Mutex 與 Binary Semaphore 的差別：mutex 有「擁有者」（誰 Acquire 就要由誰 Release），
 *      且有優先權繼承（Priority Inheritance）可避免優先權反轉，適合拿來保護資源；
 *      semaphore 沒有擁有者，適合拿來做 ISR/Task 之間的通知。
 * ==========================================================================
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
/* 放進 Task1Queue 的訊息內容：按鈕是短按還是長按 */
typedef enum
{
	Short=0,   // 按住時間 < LONG_PRESS_MS
	Long=1     // 按住時間 >= LONG_PRESS_MS
}Msg_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BTN_PRESSED_LEVEL  GPIO_PIN_RESET   // 按鈕按下時腳位為低電位（上拉接法）
#define LONG_PRESS_MS      1000             // 按住超過 1000ms 視為長按
#define DEBOUNCE_MS        150              // 消抖等待時間
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
/* Queue：Task_BtnDebounce → Task_1，傳遞 Short/Long 結果 */
osMessageQueueId_t Task1QueueHandle;

/* 按鈕消抖 Task（手動新增，不是 CubeMX 產生的） */
osThreadId_t BtnDebounceTaskHandle;
const osThreadAttr_t BtnDebounceTask_attributes = {
  .name = "BtnDebounceTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};

/*
 * 以下變數會同時被 ISR 和 Task 存取，所以加 volatile，
 * 避免編譯器最佳化時把值暫存在暫存器裡，導致讀不到最新值。
 */
static volatile uint32_t pressTick = 0;        // 確認「按下」時的 tick，用來計算按住多久
static volatile uint8_t  isPressed = 0;        // 1=目前處於按下狀態，等待放開
static volatile uint8_t  debounceLocked = 0;   // 1=正在等確認結果，忽略新邊緣
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
void LED_Blink(int,int);
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
  /* add mutexes, ... */
  /*
   * myMutex01：保護共享資源 LED2。
   * Task_1（按鈕閃燈）和 Task_2（定時閃燈）都會 toggle LED2，
   * 閃燈前先 Acquire、閃完再 Release，確保兩者不會同時操作 LED2。
   */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /*
   * osSemaphoreNew(最大值, 初始值, 屬性)
   * 兩個都是 Binary Semaphore：最大值 1、初始值 0。
   *   初始值 0 → Task 一開始 Acquire 就會 Blocked，直到有人 Release（0→1）才被喚醒，
   *              喚醒時 Acquire 把 count 拿走（1→0），下一輪又會回到等待。
   *   最大值 1 → 在 Task 還沒來得及 Acquire 前連續 Release 多次，也只會記成 1 次。
   *
   * Task2Sem：PeriodicTimer callback → Task_2，通知「10 秒到了，該閃燈了」
   * Task1Sem：按鈕 EXTI ISR → Task_BtnDebounce，通知「按鈕腳位有邊緣變化」
   */
  Task2SemHandle = osSemaphoreNew(1, 0, &Task2Sem_attributes);
  Task1SemHandle = osSemaphoreNew(1, 0, &Task1Sem_attributes);   // 拿回來用，當 ISR→Task 的通知
  /* USER CODE END RTOS_SEMAPHORES */

  /* Create the timer(s) */
  /* creation of PeriodicTimer */
  PeriodicTimerHandle = osTimerNew(Callback01, osTimerPeriodic, NULL, &PeriodicTimer_attributes);

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* 週期性軟體計時器：每 10000ms 呼叫一次 Callback01 */
  osTimerStart(PeriodicTimerHandle, 10000);

  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /*
   * Task1Queue：Task_BtnDebounce → Task_1
   *   可存 4 筆，每筆大小 sizeof(uint8_t)，內容是 Msg_t（Short / Long）。
   *   用 queue 而不是 semaphore，是因為除了「有按按鈕」之外，還要把「長按或短按」的資訊一起傳過去。
   *   Task_1 閃燈期間若又有按鍵結果，會先排在 queue 裡不會遺失。
   */
  Task1QueueHandle = osMessageQueueNew(4, sizeof(uint8_t), NULL);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of Task02 */
  Task02Handle = osThreadNew(Task_2, NULL, &Task02_attributes);

  /* creation of Task01 */
  Task01Handle = osThreadNew(Task_1, NULL, &Task01_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* 按鈕消抖 Task：接 ISR 的通知，判斷長按/短按後丟進 queue */
  BtnDebounceTaskHandle = osThreadNew(Task_BtnDebounce, NULL, &BtnDebounceTask_attributes);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_Task_2 */
/**
  * @brief  Task_2：定時閃燈 Task
  *         1. Acquire Task2Sem 等待 PeriodicTimer 的通知（count 0 時 Blocked，不佔 CPU）
  *         2. 被喚醒後（count 1→0）先 Acquire mutex，取得 LED2 使用權
  *         3. LED2 以 10Hz 閃 2 秒
  *         4. Release mutex，讓 Task_1 也能使用 LED2
  *         5. 回到步驟 1 等下一次 10 秒
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_Task_2 */
void Task_2(void *argument)
{
  /* USER CODE BEGIN Task_2 */
  /* Infinite loop */
    while (1)
    {
        // 等待 Timer 通知：count 為 0 就 Blocked；Timer Release 後 count 1→0，往下執行
        osSemaphoreAcquire(Task2SemHandle, osWaitForever);   // 等 Timer 通知
        // 取得 LED2 使用權；若 Task_1 正在閃燈，這裡會等到 Task_1 Release 為止
        osMutexAcquire(myMutex01Handle, osWaitForever);      // 取得 LED2 使用權
        LED_Blink(10, 2000);                                 // 10Hz, 2 秒
        // 用完立刻歸還，否則 Task_1 會一直拿不到 LED2
        osMutexRelease(myMutex01Handle);
    }
  /* USER CODE END Task_2 */
}

/* USER CODE BEGIN Header_Task_1 */
/**
  * @brief  Task_1：按鈕閃燈 Task
  *         1. 從 Task1Queue 取出訊息（queue 空的時候 Blocked 等待）
  *         2. 取得 mutex（LED2 使用權）
  *         3. 依訊息內容閃燈：Long → 10Hz 閃 5 秒；Short → 1Hz 閃 5 秒
  *         4. Release mutex
  *         消抖與長短按判斷都在 Task_BtnDebounce 完成，這裡只負責「依結果閃燈」。
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_Task_1 */
void Task_1(void *argument)
{
  /* USER CODE BEGIN Task_1 */
  uint8_t msg;   // 從 queue 收到的 Msg_t（Short / Long）
  while(1)
  {
    // 等待 queue 裡有資料；取出後 msg 就是長按/短按的結果
    if (osMessageQueueGet(Task1QueueHandle, &msg, NULL, osWaitForever) == osOK)
    {
      // 取得 LED2 使用權；若 Task_2 正在閃燈，這裡會等 Task_2 閃完
      osMutexAcquire(myMutex01Handle, osWaitForever);
      if (msg == Long)
        LED_Blink(10, 5000);   // 長按：10Hz 閃 5 秒
      else
        LED_Blink(1, 5000);    // 短按：1Hz  閃 5 秒
      osMutexRelease(myMutex01Handle);
    }
  }
  /* USER CODE END Task_1 */
}

/* Callback01 function */
void Callback01(void *argument)
{
  /* USER CODE BEGIN Callback01 */
  /*
   * PeriodicTimer 每 10 秒呼叫一次。
   * 軟體計時器的 callback 是在 Timer Service Task 裡執行，所有軟體計時器共用這個 Task，
   * 所以不能在這裡阻塞（不能 osDelay、不能等 mutex），否則其他計時器都會被卡住。
   * 因此只做 Release（count 0→1）通知 Task_2，閃燈的工作交給 Task_2。
   */
osSemaphoreRelease(Task2SemHandle);
  /* USER CODE END Callback01 */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */

/*
 * LED_Blink：讓 LED2 以 f Hz 閃爍 duration_ms 毫秒
 *   一個週期 = 亮 + 滅 = 2 次 toggle，所以每次 toggle 間隔半週期 = 1000/(2f) ms
 *   例：f=10 → half_period=50ms，duration=2000 → toggle 40 次
 *       f=1  → half_period=500ms，duration=5000 → toggle 10 次
 *   用 osDelay 而不是 HAL_Delay：osDelay 會讓出 CPU 給其他 Task，HAL_Delay 則是空轉。
 *   注意：呼叫前必須先取得 myMutex01，因為 LED2 是共享資源。
 */
void LED_Blink(int f,int duration_ms )
{
	int half_period=1000/(2*f);
	int count=duration_ms/half_period;
  for(int i=0;i<count;i++)
  {
    HAL_GPIO_TogglePin(GPIOB,LED2_Pin);
    osDelay(half_period);
  }
}

/*
 * Task_BtnDebounce：按鈕消抖 + 長按/短按判斷
 *   1. Acquire Task1Sem 等 ISR 通知「腳位有邊緣」（count 1→0）
 *   2. osDelay(DEBOUNCE_MS) 等彈跳結束（這段期間 debounceLocked=1，ISR 會忽略彈跳產生的邊緣）
 *   3. 讀腳位的穩定狀態：
 *        - 是按下 → 記錄按下時間 pressTick，isPressed=1
 *        - 是放開且之前有按下 → 計算按住時間，決定 Short/Long，放進 Task1Queue 給 Task_1
 *   4. debounceLocked=0，開放 ISR 接受下一次邊緣
 *
 *   為什麼消抖要放在 Task 而不是 ISR：
 *     ISR 裡不能 osDelay，也不應該空等，所以 ISR 只通知，等待與判斷都在 Task context 做。
 *   按下和放開都各延遲了 DEBOUNCE_MS，兩者相減時延遲會抵銷，不影響按住時間的計算。
 */
void Task_BtnDebounce(void *argument)
{
  for (;;)
  {
    osSemaphoreAcquire(Task1SemHandle, osWaitForever);   // 等 ISR 通知有邊緣發生
    osDelay(DEBOUNCE_MS);                                 // Task context，這裡用 osDelay 完全合法

    uint32_t now   = osKernelGetTickCount();              // 目前 tick（ms）
    uint8_t  state = HAL_GPIO_ReadPin(UserButton_GPIO_Port, UserButton_Pin);  // 彈跳結束後的穩定狀態

    if (state == BTN_PRESSED_LEVEL)
    {
      // 確認為「按下」：記錄開始時間，等之後放開時計算按了多久
      pressTick = now;
      isPressed = 1;
    }
    else if (isPressed&&HAL_GPIO_ReadPin(UserButton_GPIO_Port, UserButton_Pin)==GPIO_PIN_SET)
    {
      // 確認為「放開」且之前有按下：計算按住時間並分類
      isPressed = 0;
      uint8_t msg = ((now - pressTick) >= LONG_PRESS_MS) ? Long : Short;
      // 把結果放進 queue 通知 Task_1；timeout=0 表示 queue 滿了就直接丟掉，不讓消抖 Task 卡住
      osMessageQueuePut(Task1QueueHandle, &msg, 0, 0);
    }

    debounceLocked = 0;   // 這次確認流程結束，才開放接受下一次邊緣
  }
}

/*
 * HAL_GPIO_EXTI_Callback：按鈕外部中斷（ISR context）
 *   EXTI 需設為 Rising + Falling 雙邊觸發，按下、放開都會進來。
 *   ISR 原則是「越短越好」：不判斷按下還是放開、不延遲，只負責通知 Task_BtnDebounce。
 *   debounceLocked：第一個邊緣進來後上鎖，彈跳產生的後續邊緣直接忽略，
 *                   直到 Task_BtnDebounce 確認完畢才解鎖。
 *   注意：要在 ISR 呼叫 RTOS API，此 EXTI 的 NVIC 優先權數值必須 >=
 *         configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY（數字越大優先權越低）。
 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != UserButton_Pin)   // 不是按鈕的中斷就不處理
        return;
    if (debounceLocked)               // 正在消抖中，忽略彈跳邊緣
        return;

    debounceLocked = 1;
    osSemaphoreRelease(Task1SemHandle);   // count 0→1 喚醒 Task_BtnDebounce；這是合法的 ISR-safe API
}

/* USER CODE END Application */
