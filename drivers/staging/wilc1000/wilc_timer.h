#ifndef __WILC_TIMER_H__
#define __WILC_TIMER_H__

/*!
 *  @file	wilc_timer.h
 *  @brief	Timer (One Shot and Periodic) OS wrapper functionality
 *  @author	syounan
 *  @sa		wilc_oswrapper.h top level OS wrapper file
 *  @date	16 Aug 2010
 *  @version	1.0
 */

#include "wilc_platform.h"
#include "wilc_errorsupport.h"

typedef void (*tpfWILC_TimerFunction)(void *);

/*!
 *  @brief	Creates a new timer
 *  @details	Timers are a useful utility to execute some callback function
 *              in the future.
 *              A timer object has 3 states : IDLE, PENDING and EXECUTING
 *              IDLE : initial timer state after creation, no execution for the
 *              callback function is planned
 *              PENDING : a request to execute the callback function is made
 *              using WILC_TimerStart.
 *              EXECUTING : the timer has expired and its callback is now
 *              executing, when execution is done the timer returns to PENDING
 *              if the feature CONFIG_WILC_TIMER_PERIODIC is enabled and
 *              the flag tstrWILC_TimerAttrs.bPeriodicTimer is set. otherwise the
 *              timer will return to IDLE
 *  @param[out]	pHandle handle to the newly created timer object
 *  @param[in]	pfEntry pointer to the callback function to be called when the
 *              timer expires
 *              the underlaying OS may put many restrictions on what can be
 *              called inside a timer's callback, as a general rule no blocking
 *              operations (IO or semaphore Acquision) should be perfomred
 *              It is recommended that the callback will be as short as possible
 *              and only flags other threads to do the actual work
 *              also it should be noted that the underlaying OS maynot give any
 *              guarentees on which contect this callback will execute in
 *  @return	Error code indicating sucess/failure
 *  @sa		WILC_TimerAttrs
 *  @author	syounan
 *  @date	16 Aug 2010
 *  @version	1.0
 */
WILC_ErrNo WILC_TimerCreate(struct timer_list *pHandle,
			    tpfWILC_TimerFunction pfCallback);

/*!
 *  @brief	Starts a given timer
 *  @details	This function will move the timer to the PENDING state until the
 *              given time expires (in msec) then the callback function will be
 *              executed (timer in EXECUTING state) after execution is dene the
 *              timer either goes to IDLE (if bPeriodicTimer==false) or
 *              PENDING with same timeout value (if bPeriodicTimer==true)
 *  @param[in]	pHandle handle to the timer object
 *  @param[in]	u32Timeout timeout value in msec after witch the callback
 *              function will be executed. Timeout value of 0 is not allowed for
 *              periodic timers
 *  @return	Error code indicating sucess/failure
 *  @sa		WILC_TimerAttrs
 *  @author	syounan
 *  @date	16 Aug 2010
 *  @version	1.0
 */
WILC_ErrNo WILC_TimerStart(struct timer_list *pHandle, u32 u32Timeout, void *pvArg);

#endif
