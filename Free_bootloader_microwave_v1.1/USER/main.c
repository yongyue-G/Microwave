#include "led.h"
#include "delay.h"
#include "key.h"
#include "sys.h"
#include "usart.h"
#include "timer.h"
#include "FreeRTOS.h"
#include "exti.h"
#include "lcd.h"
#include "gui.h"
#include "beep.h"
#include "dma.h"
#include "stmflash.h"
#include "iap.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "micro_wave_oven.h"
#include "dht11.h"


/*************************************************************************
*项目名：基于FreeRTOS和STM32的微波炉(bootloader程序)
*实现功能：请看用户手册
*作者：不甘心的咸鱼--闲鱼
*版本：V1.1
*闲鱼号：tb43915564
*修改日期：2025/3/20
*请勿商用！违者必究！
**************************************************************************/



/////////////////////////FreeRTOS任务设置////////////////////////////////////////////////////////////////////////////////////////
//START 任务
//设置任务优先级
#define START_TASK_PRIO      			1 /*开始任务的优先级设置为最低,与ucos只能相反，此处0为最低优先级，赋给IDLE任务*/
//设置任务堆栈大小
#define START_STK_SIZE  				64 /*单位是字，一个字是4个字节，半字是2个字节*/
//任务句柄
TaskHandle_t StartTask_Handler;
//任务函数
static void start_task(void *pvParameters);	

//DHT11任务
//设置任务优先级
#define dht11_task_prio       			2
//设置任务堆栈大小
#define dht11_stk_size  				128
//任务句柄
TaskHandle_t  dht11_task_handler;
//任务函数
static void dht11_task(void *pvParameters);

//KEY1按键任务
//设置任务优先级
#define KEY1_TASK_PRIO       			4
//设置任务堆栈大小
#define KEY1_STK_SIZE  					256
//任务句柄
TaskHandle_t  KEY1Task_Handler;
//任务函数
static void key1_task(void *pvParameters);


//KEY2按键任务
#define KEY2_TASK_PRIO					3
#define	KEY2_STK_SIZE					256
TaskHandle_t  KEY2Task_Handler;
static void   key2_task(void *pvParameters);


//加热任务
#define cooking_task_prio   			5
#define cooking_task_size				128
TaskHandle_t  CookingTask_Handler;
static void   cooking_task(void *pvParameters);


#if ifopen
//IAP任务
#define  iap_task_prio               	6
#define  iap_task_size            		512
TaskHandle_t IapTask_Handler;
static void  iap_task(void *pvParameters);

#endif
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


//////////////////////////定义全局变量///////////////////////////////////////////////////////////
//定义需要用到的全局变量
static SemaphoreHandle_t 	key1_sem;
static SemaphoreHandle_t 	key2_sem;
static SemaphoreHandle_t 	heating_sem;
#if ifopen
static SemaphoreHandle_t 	sig5;
#endif
led_d led1;							/*led结构体变量--不甘心的咸鱼注*/
led_d bep;							/*beep结构体变量--不甘心的咸鱼注*/
static led_d p14;					/*DHT11结构体变量*/
static MicrowaveCtrl microwave;		/*微波炉结构体变量*/	
/********************************************************************************************/


/******************************定义APP程序缓冲区大小************************************************************************************************/
/**若使能为1，此时点击MAP文件会发现RAM大小为19.56KB，这是因为boot偏移了16K，再加上APP的3.56K，因此这里的RAM是boot加APP的合计***/
//定义接收的APP代码大小
#if ifopen		/*如要使用固件更新功能，请使能该宏为1*/

	/*需要注意不能让bootloader程序的RAM区域与APP程序的RAM区域重叠，而笔者bootloader的RAM大概为12K，因此偏移了16K，为0x4000,剩下的4Kram给APP*/
	
	u8 receive_buff[buff_size]  __attribute__ ((at(0X20004000)));/*将数组分配到固定的内存地址（请根据需要修改）,否则会判断固件程序失败*/
	
#endif

/***---------------------------------------------------------------------------------------------------------------------------------------------***/



/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: 主函数，用于硬件初始化并创建起始任务
*@para: void
*@return: none
**/
 int main(void)
 {	
	/************************************************************************************************************/
	/*由于共计4个中断，设置中断分组为4，4位抢占优先级，0位亚优先级，因为Free不支持亚优先级处理。
	 由于Free中存在屏蔽优先级阈值的概念，该工程笔者设置为3，因此我们需要调用API函数的中断优先级不能超过3，
	 所以DMA中断优先级为4，定时器2中断优先级为5，PB12外部中断优先级6，PB1外部中断优先级7，从优先级都设置为0--不甘心的咸鱼注*/
	/*************************************************************************************************************/
	NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);//设置中断优先级分组为组4：4位抢占优先级，0位响应优先级 	
	delay_init();	    						//延时函数初始化
	uart_init(115200);
	LED_Init(&led1,GPIOC,GPIO_Pin_13);			//初始化与LED连接的硬件接口
	Beep_Init(&bep,GPIOB,GPIO_Pin_15);			/*初始化beep*/
	 
	 while(1)
	 {
		buzzer_beep(8);
	 
	 }
	TIM3_PWM_Init(99,14399);					/*初始化转盘*/
	TIM2_PWM_Init(99,14399);					/*初始化加热片及微波炉门*/
	EXTIX_Init();								/*初始化按键中断*/
	TIM4_Int_Init(10000-1,7200-1);				/*初始化定时器用于微波炉倒计时，定时1s*/
	LCD_Init();									/*LCD初始化*/
	micro_wave_init(&microwave);				/*微波炉初始化*/ 
#if ifopen
	MYDMA_Config(DMA1_Channel5,(u32)&USART1->DR,(u32)receive_buff,buff_size);//初始化DMA
#endif  
	xTaskCreate(start_task,"start_task",START_STK_SIZE,NULL,START_TASK_PRIO,&StartTask_Handler);/*创建起始任务*/
	vTaskStartScheduler(); 	/*开启多任务调度*/  	 
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/


/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: 开始任务，用于创建其他任务
*@para: *pvParameters
*@return: none
**/	  
static void start_task(void *pvParameters)
{
	taskENTER_CRITICAL();			//进入临界区(无法被中断打断) 
	
	/*****创建二值信号量--FreeRTOS********/
	key1_sem=xSemaphoreCreateBinary();
	key2_sem=xSemaphoreCreateBinary();
	heating_sem=xSemaphoreCreateBinary();
#if ifopen
	sig5=xSemaphoreCreateBinary();
#endif
	/**************************************/
	
	/********创建任务**************************************************************************************/   
	xTaskCreate(dht11_task,"dht11_task",dht11_stk_size,NULL,dht11_task_prio,&dht11_task_handler);
	xTaskCreate(key1_task,"key1_task",KEY1_STK_SIZE,NULL,KEY1_TASK_PRIO,&KEY1Task_Handler);
	xTaskCreate(key2_task,"key2_task",KEY2_STK_SIZE,NULL,KEY2_TASK_PRIO,&KEY2Task_Handler);
	xTaskCreate(cooking_task,"cooking_task",cooking_task_size,NULL,cooking_task_prio,&CookingTask_Handler);
#if ifopen
	xTaskCreate(iap_task,"iap_task",iap_task_size,NULL,iap_task_prio,&IapTask_Handler);
#endif
	vTaskDelete(StartTask_Handler); 	//删除开始任务
	/****************************************************************************************************/
	
	taskEXIT_CRITICAL();				//退出临界区(可以被中断打断)
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/



/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: DHT11任务
*@para: *pvParameters
*@return: none
**/
static void dht11_task(void *pvParameters)
{
		u8 temp,humi;
		while(1)
		{
			/*读取温度值*/
			DHT_Read_Data(&temp,&humi,GPIOC,GPIO_Pin_14,&p14);
				
			/*显示温度数据*/
			Show_Str(0,80,BLUE,WHITE,"Temp:00C",16,0);
			LCD_ShowNum(0+5*8,80,temp,2,16);
				
			/*仅在加热状态下处理温度异常*/
			if(microwave.isheating){
				
				/*温度过高，触发报警措施*/
				if(temp >= 30){
					/*结束加热*/
					stop_cooking(&microwave);
					
					/*鸣叫四次*/
					buzzer_beep(4);
					
					/*开门开灯*/
					end_cooking();
					
					/*重置状态*/
					microwave.state=STATE_STANDBY;
					
					/*重置门状态*/
					microwave.door_state=0;
					
					/*显示温度过高*/
					LCD_Fill(0,140,128,160,WHITE);	//清屏
					Show_Str(0,140,BLUE,WHITE,"Over heat!",16,0);
				}
			}
			delay_ms(500);
		}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/


/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: 微波炉加热任务
*@para: *pvParameters
*@return: none
**/	  
static void cooking_task(void *pvParameters)
{
	
	while(1)
	{
		xSemaphoreTake(heating_sem, portMAX_DELAY);/*等待启动加热后才开始读取温度值*/
		
		/*确保在加热状态下*/
		if(microwave.isheating){
			/*倒计时显示*/
			if(microwave.cooking_time>0){
					
					/*倒计时递减*/
					microwave.cooking_time--;
					
					/*虽然调用后会一直存在LCD上，但是如果不更新，会一直显示旧值*/
					LCD_ShowNum(0+5*8,40,microwave.cooking_time,3,16);
			}
			else{
				/*倒计时归零*/
				stop_cooking(&microwave);
				
				/*蜂鸣器短鸣三声代表结束*/
				buzzer_beep(3);
				
				/*显示完成状态*/
				LCD_Fill(0,140,128,160,WHITE);	//清屏
				Show_Str(0,140,BLUE,WHITE,"Completed",16,0);
				
				/*重置功率*/
				microwave.power=POWER_MEDIUM;
				
				/*显示重置功率*/
				power_display(&microwave);
				
				/*显示重置时间*/
				LCD_ShowNum(0+5*8,40,microwave.cooking_time,3,16);
				
				/*加热完成*/
				end_cooking();
				
				/*切换为完成状态*/
				microwave.state=STATE_COMPLETED;
				
				/*重置门状态*/
				microwave.door_state=0;
			}
		}
	}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/


/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: KEY1按键任务，根据长按还是短按进行开关门/加热操作
*@para: *pvParameters
*@return: none
**/	  
static void key1_task(void *pvParameters)
{
    u32 ticksStart;       // 按键按下时的时钟节拍
    u32 pressDuration;    // 按键持续时间
    u8 longPressHandled;  // 长按是否已处理标志
    
    while(1)
    {
        /* ----- KEY1处理 ----- */
        /* 无限等待KEY1信号量，任务将阻塞在此直到接收到信号量 */
        if(xSemaphoreTake(key1_sem, portMAX_DELAY) == pdTRUE)
        {
            /* 记录按下开始时间 */
            ticksStart = xTaskGetTickCount();
            longPressHandled = 0;   /*长按处理标志，如果没有该标志的话，长按会一直处理，就会一直启动加热*/
            
            /* 持续检测按键状态，直到释放 */
            while(1)
            {
                /* 判断按键是否已释放 */
                if(KEY1!= 0)
                {
                    /* 计算按键持续时间 */
                    pressDuration = xTaskGetTickCount() - ticksStart;
                    
                    /* 如果没有处理过长按且未达到长按时间，则处理短按 */
                    if(!longPressHandled && pressDuration <= 1000)
                    {
                        /* 短按KEY1 - 开关门逻辑 */
                        microwave.door_state = !microwave.door_state;
                        
                        if(microwave.door_state)/*如果为关门状态*/
                        {
							Beep_off(&bep);/*关闭蜂鸣器*/
							plan_cooking();
							
							/*清屏*/
							LCD_Fill(0,0,128,35,WHITE);
							LCD_Fill(0,140,128,160,WHITE);
							LCD_Fill(0,100,128,115,WHITE);
							
							/* 更新显示 */
							LCD_Fill(0,0,128,35,WHITE);//清屏
							Show_Str(0,20,BLUE,WHITE,"door closed",16,0);
							
							/*重置状态*/
							if(microwave.state==STATE_COMPLETED){
								microwave.state=STATE_STANDBY;
							}
                        }
                        else
                        {
							end_cooking();
							
							/* 更新显示 */
							LCD_Fill(0,0,128,35,WHITE);//清屏
							Show_Str(0,20,BLUE,WHITE,"door open",16,0);
							
                            /* 如果正在加热，停止加热 */
                            if(microwave.isheating)
                            {
                                stop_cooking(&microwave);
								Beep_on(&bep);/*蜂鸣器长鸣*/
                            }
          
                        }
                    }
                    /* 按键释放，跳出检测循环 */
                    break;
                }
                else
                {
                    /* 计算当前持续时间 */
                    pressDuration = xTaskGetTickCount() - ticksStart;
                    
                    /* 检测是否满足长按条件且尚未处理长按 */
                    if(pressDuration > 1000 && !longPressHandled)
                    {
                        longPressHandled = 1; /* 设置长按已处理标志 */
                        
                        /* 长按KEY1 - 开始加热 */
                        if(microwave.door_state && !microwave.isheating && 
                           (microwave.state == STATE_TIME_SETTING || 
                            microwave.state == STATE_POWER_SETTING || 
                            microwave.state == STATE_STANDBY))
                        {
							/*清屏*/
							LCD_Fill(0,100,128,115,WHITE);	
							LCD_Fill(0,140,128,160,WHITE);	
							
							/* 蜂鸣器响两声表示开始加热 */
							buzzer_beep(2);
							
							/*启动微波炉加热*/
							start_cooking(&microwave);
                        }
						
						/*门未关闭时启动*/
						else if(!microwave.door_state){
							stop_cooking(&microwave);//有必要吗？？？？？？？？
							buzzer_beep(1);	/*蜂鸣器短鸣一次*/
							LCD_Fill(0,140,128,160,WHITE);	//清屏
							Show_Str(0,140,BLUE,WHITE,"Close door",16,0);
						}
                    }
                }
                
                /* 短暂延时，避免占用CPU */
                delay_ms(10);   /*防止当一直长按时，CPU一直被占用，让其他任务饿死*/
            }
        }
	}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/     


/***---------------------------------------------------------------------------------------------------------------------------------------------***/ 
/**
*@brief: KEY2按键任务，根据长按还是短按进行挡位及倒计时切换
*@para: *pvParameters
*@return: none
**/	  
static void key2_task(void *pvParameters)
{
	u32 ticksStart;       // 按键按下时的时钟节拍
    u32 pressDuration;    // 按键持续时间
    u8 longPressHandled;  // 长按是否已处理标志
	
	while(1)
	{
		/* ----- KEY2处理 ----- */
        /* 无限等待KEY2信号量 */
        if(xSemaphoreTake(key2_sem, portMAX_DELAY) == pdTRUE)
        {
            /* 记录按下开始时间 */
            ticksStart = xTaskGetTickCount();
            longPressHandled = 0;
            
            /* 持续检测按键状态，直到释放 */
            while(1)
            {
                /* 判断按键是否已释放 */
                if(KEY2 != 0)
                {
                    /* 计算按键持续时间 */
                    pressDuration = xTaskGetTickCount() - ticksStart;
                    
                    /* 如果没有处理过长按且未达到长按时间，则处理短按 */
                    if(!longPressHandled && pressDuration <= 1000)
                    {
                        /* 短按KEY2 - 时间和功率设置 */
                        if(microwave.state == STATE_TIME_SETTING)
                        {
                            microwave.cooking_time = (microwave.cooking_time+20);
							if(microwave.cooking_time>200){
								microwave.cooking_time=0;
							}
							/*显示时间*/
							LCD_ShowNum(0+5*8,40,microwave.cooking_time,3,16);
                        }
                        else if(microwave.state == STATE_POWER_SETTING)
                        {
                            /* 切换功率等级 */
                            microwave.power = (PowerLevel)((microwave.power + 1) % 3);
                            power_display(&microwave);
                        }
                        else if(microwave.state == STATE_STANDBY || 
                                microwave.state == STATE_COMPLETED)
                        {
                            /* 进入时间设置模式 */
                            microwave.state = STATE_TIME_SETTING;
							/*显示时间设置*/
							LCD_Fill(0,100,128,115,WHITE);	//清屏
							Show_Str(0,100,BLUE,WHITE,"Time set",16,0);
                        }
                    }
                    /* 按键释放，跳出检测循环 */
                    break;
                }
                else
                {
                    /* 计算当前持续时间 */
                    pressDuration = xTaskGetTickCount() - ticksStart;
                    
                    /* 检测是否满足长按条件且尚未处理长按 */
                    if(pressDuration > 1000 && !longPressHandled)
                    {
                        longPressHandled = 1; /* 设置长按已处理标志*/
                        
                        /* 长按KEY2 - 切换到功率设置 */
                        if(microwave.state == STATE_TIME_SETTING)
                        {
                            microwave.state = STATE_POWER_SETTING;
							
							/*显示功率设置*/
							LCD_Fill(0,100,128,115,WHITE);	//清屏
							Show_Str(0,100,BLUE,WHITE,"Power set",16,0);
                        }
						else if(microwave.state==STATE_POWER_SETTING)
						{
							
							/*切换状态*/
							microwave.state = STATE_TIME_SETTING;
							
							/*显示功率设置*/
							LCD_Fill(0,100,128,115,WHITE);	//清屏
							Show_Str(0,100,BLUE,WHITE,"Time set",16,0);
						}
                    }
                }
                /* 短暂延时，避免一直长按占用CPU */
                delay_ms(10);
            }
        }
	}
}

/***---------------------------------------------------------------------------------------------------------------------------------------------***/ 



/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: KEY2（PB1）按键中断，释放信号量
*@para: void
*@return: none
**/	  
void EXTI1_IRQHandler(void)//外部中断1,按下加速
{
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;/*释放二值信号量所需参数，初始化为pdFalse*/
		
		/* 检查是否是EXTI1中断 */
		if(EXTI_GetITStatus(EXTI_Line1) != RESET)
		{
			/* 简单延时消抖 */
			delay_xms(10);
			
			/* 检查是否为按键按下状态 */
			if(KEY2 == 0)
			{
				/* 发送信号量通知按键任务 */
				xSemaphoreGiveFromISR(key2_sem, &xHigherPriorityTaskWoken);
			}
			
			/* 清除中断标志位 */
			EXTI_ClearITPendingBit(EXTI_Line1);
			
			/* 根据需要进行任务切换 */
			portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
		}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/

/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: KEY1（PB12）按键中断，释放信号量
*@para: void
*@return: none
**/	  
void EXTI15_10_IRQHandler(void)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;/*释放二值信号量所需参数，初始化为pdFalse*/
	if(EXTI_GetITStatus(EXTI_Line12)!=RESET)
	{
		
	  delay_xms(10);	/*消抖,不会引发调度*/
		
	  if(KEY1==0){	
		  
			xSemaphoreGiveFromISR(key1_sem, &xHigherPriorityTaskWoken);/*释放二值信号量*/ 
		  
	    }
	  EXTI_ClearITPendingBit(EXTI_Line12);
		
	  /* 如果需要的话进行一次任务切换 */
	  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/


/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: TIM4中断，用于微波炉倒计时
*@para: void
*@return: none
**/	  
void TIM4_IRQHandler(void)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;/*释放二值信号量所需参数，初始化为pdFalse*/
	
	if (TIM_GetITStatus(TIM4, TIM_IT_Update) != RESET) //检查 TIM4 更新中断发生与否
	{
			TIM_ClearITPendingBit(TIM4, TIM_IT_Update ); //清除 TIM4 更新中断标志
			//led_on(&led1);/*调试用*/
			xSemaphoreGiveFromISR(heating_sem, &xHigherPriorityTaskWoken);/*释放二值信号量*/ 
		
			/* 如果需要的话进行一次任务切换 */
			portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
	}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/

/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: Bootloader任务,用于固件升级
*@para: *pvParameters
*@return: none
**/	  
#if ifopen
static void iap_task(void *pvParameters)
{
	uint16_t receivedLength;
	while(1)
	{
		xSemaphoreTake(sig5,portMAX_DELAY);	/*等待信号量，portMAX_DELAY代表无限等*/
		//printf("执行到这里");//调试用
		receivedLength = GetReceivedDataLength();/*计算所接收的数据量，需要将该变量放在等待信号量函数后，以确保及时更新*/
		if(receive_buff[0])//看是否接收到了APP代码
		{
				printf("已接收到APP代码为%d\n",receivedLength);
				//printf("%s\n",receive_buff);//调试用
				/*拷贝、跳转*/
				if(((*(vu32*)(0X20004000+4))&0xFF000000)==0x08000000)//判断是否为0X08XXXXXX.
					{	 
						Show_Str(0,120,BLUE,WHITE,"Firmware updating!",16,0);/*打印信息在LCD屏幕上代表正在更新*/
						FLASH_ErasePage(FLASH_APP1_ADDR);/*先擦除APP区的flash，防止有数据残留--不甘心的咸鱼*/
						iap_write_appbin(FLASH_APP1_ADDR,receive_buff,buff_size);//更新FLASH代码   				
						printf("固件更新完成!\r\n");	
						printf("开始执行flash用户代码\r\n");
						if(((*(vu32*)(FLASH_APP1_ADDR+4))&0xFF000000)==0x08000000)//判断是否为0X08XXXXXX.
							{	 
									iap_load_app(FLASH_APP1_ADDR);//执行FLASH APP代码
							}
						else{
									printf("非FLASH应用程序,无法执行!\r\n");  
							}	
					}
					/*如果判断固件失败，先将接收APP数组情况，防止数据残留并重新开启DMA，用于下次更新--不甘心的咸鱼*/
				else{
						memset(receive_buff,0,buff_size);   //清空数组
						MYDMA_Enable(DMA1_Channel5);//开启一次DMA传输，用于下次更新
						printf("error!固件判断失败");
					}
		}
	}
}
/***---------------------------------------------------------------------------------------------------------------------------------------------***/


/***---------------------------------------------------------------------------------------------------------------------------------------------***/
/**
*@brief: DMA中断任务，用于接收APP程序
*@para: *pvParameters
*@return: none
**/	  
void DMA1_Channel5_IRQHandler(void) 
{
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;/*释放二值信号量所需参数，初始化为pdFalse*/
		// 检查DMA传输完成中断
		if (DMA_GetITStatus(DMA1_IT_TC5))
		{
        // 清除中断标志
        DMA_ClearITPendingBit(DMA1_IT_TC5);
		//printf("CNDTR: %d\n", DMA_GetCurrDataCounter(DMA1_Channel5)); // 调试用
		xSemaphoreGiveFromISR(sig5, &xHigherPriorityTaskWoken);/*释放二值信号量*/	
		}
		/* 如果需要的话进行一次任务切换 */
		portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
#endif
/***---------------------------------------------------------------------------------------------------------------------------------------------***/

