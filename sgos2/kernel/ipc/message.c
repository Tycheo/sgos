//message management.

#include <sgos.h>
#include <stdlib.h>
#include <mm.h>
#include <kd.h>
#include <ipc.h>
#include <tm.h>

// 删除一个消息
static void MessageEraser(const void * p)
{
	KMessage* kmsg = (KMessage*)p;
	MmFreeKernelMemory( kmsg );
}

// 给出source线程查找一个消息
static int MessageSearcher1(const void * p, const void *q )
{
	if( ((KMessage*)p)->Source->ThreadId == ((Message*)q)->ThreadId )
		return 1;
	return 0;	//search next
}
// 给出command查找一个消息
static int MessageSearcher2(const void * p, const void *q )
{
	if( ((KMessage*)p)->UserMessage.Command == ((Message*)q)->Command )
		return 1;
	return 0;	//search next
}
// 给出source线程和command查找一个消息
static int MessageSearcher3(const void * p, const void *q )
{
	if( ((KMessage*)p)->Source->ThreadId == ((Message*)q)->ThreadId &&
		((KMessage*)p)->UserMessage.Command == ((Message*)q)->Command )
		return 1;
	return 0;	//search next
}

// 线程消息初始化
void IpcInitializeThreadMessageQueue( struct KThread* thr )
{
	char tmp[16];
	sprintf( tmp, "msgQue:%x", thr->ThreadId );
	RtlCreateQueue( &thr->MessageQueue, MAX_MESSAGES_IN_QUEUE, MessageEraser, tmp, 1 );
}

// 释放消息队列占用的空间
void IpcDestroyThreadMessageQueue( struct KThread* thr )
{
	RtlDestroyQueue( &thr->MessageQueue );
}

// Quick way to send a message
int IpcQuickSend( uint tid, uint cmd, uint arg1, uint arg2 )
{
	Message msg;
	RtlZeroMemory( &msg, sizeof(Message) );
	msg.ThreadId = tid;
	msg.Command = cmd;
	msg.Arguments[0] = arg1;
	msg.Arguments[1] = arg2;
	return IpcSend( &msg, 0 );
}

// Send a message to another thread.
//发送后返回
int	IpcSend( 
	Message*	usermsg, 	//消息正文
	uint		flag		//发送参数
){
	KThread* thr_dest;
	KMessage* kmsg;
	thr_dest = TmGetThreadById( usermsg->ThreadId );
	if( thr_dest == NULL )
		return -ERR_WRONGARG;
	down( &thr_dest->Semaphore );
	// 在睡眠醒来后，目的线程可能已经终止，所以要检查状态 
	if( thr_dest->ThreadState == TS_DEAD || thr_dest->ThreadState == TS_INIT ){
		up( &thr_dest->Semaphore );
		return -ERR_WRONGARG;
	}
	kmsg = MmAllocateKernelMemory( sizeof(KMessage) );
	if( !kmsg ){
		up( &thr_dest->Semaphore );
		return -ERR_NOMEM;
	}
	//copy arguments
	kmsg->UserMessage = *usermsg;
	kmsg->Destination = thr_dest;
	kmsg->Source = TmGetCurrentThread();
	//insert message queue
	RtlPushFrontQueue( &thr_dest->MessageQueue, kmsg );
	//finished
	up( &thr_dest->Semaphore );
	//wake up the thread
	TmWakeupThread( thr_dest );
#if 0
	if( usermsg->Command != System_SleepThread )
		KdPrintf("%x sendto %x  cmd:%x arg1:%d\n", kmsg->Source->ThreadId, kmsg->Destination->ThreadId, 
			usermsg->Command, usermsg->Arguments[0] );
#endif
	return 0;	//success
}

// Receive a message
//发送后，等待对方处理完毕，超时返回负值
int	IpcCall( 
	Message*	usermsg, 	//消息正文
	uint		flag,		//发送参数
	int		timeout		//超时值
){
	int ret = IpcSend( usermsg, flag );
	if( ret!=0 || timeout==0 )
		return ret;
	TmSleepThread( TmGetCurrentThread(), timeout );
	//determine if we have got the reply message
	//PERROR("## waring!, not implemented.");
	return 0;
}

//回应IpcCall
int	IpcReply(
	Message*	usermsg,	//
	uint		flag		//
){
	KThread* thr_dest;
	thr_dest = TmGetThreadById( usermsg->ThreadId );
	//wake up the thread
	if( thr_dest )
		TmWakeupThread( thr_dest );
	return 0;	//success
}


//接收消息
//timeout为0，表示不等待，立刻返回。
//timeout为-1，表示一直等到。
//timeout非0和-1，则等待一定的毫秒。
int	IpcReceive(
	Message*	usermsg,	//消息正文缓冲区
	uint		flag,		//接收参数
	int		timeout		//超时值
){
	KThread* thr_src = NULL, * thr_cur;
	KMessage* kmsg;
	KQueueNode* nod;
	uint flags;
	int ret;
	uint startSleepTime = ArGetMilliSecond();
	if( usermsg->ThreadId != ANY_THREAD )
		thr_src = (KThread*)TmGetThreadById( usermsg->ThreadId );
	thr_cur = TmGetCurrentThread();
_recv_search:
	//锁定线程，避免在睡眠前切换了线程得到了消息
	down( &thr_cur->Semaphore );
	if( thr_src && usermsg->Command ){
		kmsg = RtlSearchQueue( &thr_cur->MessageQueue, usermsg,
			MessageSearcher3, &nod );
	}else if( thr_src ){
		kmsg = RtlSearchQueue( &thr_cur->MessageQueue, usermsg,
			MessageSearcher1, &nod );
	}else if( usermsg->Command ){
		kmsg = RtlSearchQueue( &thr_cur->MessageQueue, usermsg,
			MessageSearcher2, &nod );
	}else{	//receive any message
		kmsg = RtlPopBackQueue( &thr_cur->MessageQueue );
	}
	if( !kmsg ){
		if( timeout!=0 ){
			int rest;
			if( timeout!=INFINITE ){
				rest = timeout - (ArGetMilliSecond()-startSleepTime);
				if( rest < 0 ) //timeout happens
					return -ERR_TIMEOUT;
			}else{
				rest = INFINITE;
			}
			//禁止中断发生的理由: ???
			ArLocalSaveIrq( flags );
			up( &thr_cur->Semaphore );
			TmSleepThread( thr_cur, rest );
			ArLocalRestoreIrq( flags );
			goto _recv_search;
		}
		up( &thr_cur->Semaphore );
		//要求立即返回，但又没有取得消息
		return -ERR_NONE;
	}
	up( &thr_cur->Semaphore );
	//set return value
	ret = 1;
	//copy to user space
	*usermsg = kmsg->UserMessage;
	//check flag
	if( thr_src ){
		if( !(flag&MSG_KEEP ) ){
			RtlRemoveQueueElement( &thr_cur->MessageQueue, nod );
			MessageEraser( kmsg );
		}
	}else{
		if( flag&MSG_KEEP )
			RtlPushBackQueue( &thr_cur->MessageQueue, kmsg );
		else
			MessageEraser( kmsg );
	}
	//设置来源
	usermsg->ThreadId = kmsg->Source->ThreadId;
#if 0
	if( usermsg->Command != System_SleepThread )
		KdPrintf("%x recv from %x  cmd:%x arg1:%d\n", thr_cur->ThreadId, usermsg->ThreadId, 
			usermsg->Command, usermsg->Arguments[0] );
#endif
	return ret;
}
