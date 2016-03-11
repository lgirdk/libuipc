
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "uipc.h"

typedef struct{
	int sock;
	struct sockaddr_un servAddr;
	pthread_t threadTid;
	uIpcCmdHandler_t pHandler;
}uipcServer_t;

typedef struct{
	int sock;
	struct sockaddr_un clientAddr;
	struct sockaddr_un serverAddr;
}uipcClient_t;

static uipcServer_t gUipcServerInfo={-1};
static 	uIpcData_t uipcData;

/****************************************************************************************************************/

void uipc_svr_release_resources(void)
{
	if(gUipcServerInfo.sock >= 0){
		close(gUipcServerInfo.sock);
		gUipcServerInfo.sock = -1;
	}
	remove(gUipcServerInfo.servAddr.sun_path);
}

int uipc_svr_send_data(struct sockaddr_un *pClientAddr,uIpcData_t *pData)
{
	if((pClientAddr==NULL)||(pData==NULL)
		||(gUipcServerInfo.sock<0) ||(pData->length>UIPC_DATA_MAX_SIZE))
		return(-1);
	if(sendto(gUipcServerInfo.sock,(char *)pData,sizeof(int) + pData->length,0,(struct sockaddr *)pClientAddr,sizeof(struct sockaddr_un)) <= 0)
		return(-1);

	return(0);
}

static void * uipc_svr_thread(void *arg)
{
	unsigned char *buffer = (unsigned char *)&uipcData;
	struct sockaddr_un clientAddr;
	int rLen,fromLen;

	pthread_detach(pthread_self());

	while(1){
		fromLen = sizeof(clientAddr);
		rLen = recvfrom(gUipcServerInfo.sock,buffer,sizeof(uIpcData_t),0,(struct sockaddr *)&clientAddr,&fromLen);
		if(rLen > 0){
			/*received data from a client*/
			gUipcServerInfo.pHandler(&uipcData,&clientAddr);
		}else{
			switch(errno){
				case(EBADF):
				case(ENOTSOCK ):
					return(NULL);
				break;

				default:
					usleep(1000); /*avoid dead loop*/
				break;
			}
		}
	}
}

int uipc_svr_init(char *svrFileName,uIpcCmdHandler_t func)
{
	int sock;
	if((svrFileName==NULL)||(func==NULL))
		return(-1);
	if(gUipcServerInfo.sock >= 0)
		return(0);
	if((sock=socket(AF_UNIX, SOCK_DGRAM,0))<0){
		printf("IPC:Failed to create ipc socket\n\r");
		return(-1);
	}
	memset(&gUipcServerInfo.servAddr,0,sizeof(gUipcServerInfo.servAddr));
	gUipcServerInfo.servAddr.sun_family = AF_UNIX;
	strcpy(gUipcServerInfo.servAddr.sun_path,svrFileName);
	if(bind(sock,(struct sockaddr *)&gUipcServerInfo.servAddr,sizeof(gUipcServerInfo.servAddr))<0){
		close(sock);
		printf("UIPC:Failed to bind unix socket\n\r");
		return(-1);
	}
	gUipcServerInfo.sock = sock;
	gUipcServerInfo.pHandler = func;
	if(pthread_create(&gUipcServerInfo.threadTid,NULL,uipc_svr_thread,NULL)){
		close(sock);
		printf("UIPC:Failed to create UIPC Server thread\n\r");
		return(-1);
	}
		
	return(0);
}

static int uipc_clnt_init_sock(char *svrName, uipcClient_t *pUipcInfo)
{
	if((pUipcInfo->sock = socket(AF_UNIX, SOCK_DGRAM,0))<0){
		printf("UIPC_Client:Failed to create unix socket\n\r");
		return(-1);
	}

	memset(&pUipcInfo->clientAddr,0,sizeof(struct sockaddr_un));
	pUipcInfo->clientAddr.sun_family = AF_UNIX;
	sprintf(pUipcInfo->clientAddr.sun_path,"%s%d",UIPC_CLIENT_NAME,getpid());
	if(bind(pUipcInfo->sock,(struct sockaddr *)&pUipcInfo->clientAddr,sizeof(struct sockaddr_un)) <0 ){
		close(pUipcInfo->sock);
		pUipcInfo->sock = -1;
		printf("UIPC_Client:Failed to bind unix socket\n\r");
		return(-1);
	}

	pUipcInfo->serverAddr.sun_family = AF_UNIX;
	sprintf(pUipcInfo->serverAddr.sun_path,"%s",svrName);

	return(0);
}

static void uipc_clnt_release_resource(uipcClient_t *pUipcInfo)
{
	if(pUipcInfo->sock >= 0){
		close(pUipcInfo->sock);
		pUipcInfo->sock = -1;
	}
	if(pUipcInfo->clientAddr.sun_path[0]){
		remove(pUipcInfo->clientAddr.sun_path);
		pUipcInfo->clientAddr.sun_path[0] = 0;
	}
}

int uipc_clnt_send_data(char *svrFileName,uIpcData_t *pData)
{
	int rv =0;
	uipcClient_t clnfInfo;

	if((svrFileName==NULL)||(pData==NULL)||(pData->length>UIPC_DATA_MAX_SIZE))
		return(-1);
	memset((char *)&clnfInfo,0,sizeof(uipcClient_t));
	if(uipc_clnt_init_sock(svrFileName,&clnfInfo)){
		return(-1);
	}
	if(sendto(clnfInfo.sock,(char *)pData,sizeof(int) + pData->length,0,(struct sockaddr *)&clnfInfo.serverAddr,sizeof(struct sockaddr_un)) <= 0)
		rv = -1;

	uipc_clnt_release_resource(&clnfInfo);
	return(rv);
}

int uipc_clnt_send_and_recv_data(char *svrFileName, uIpcData_t *pData,int waitTime)
{
	int rv =0, nonBlockFlag, rLen, peerLen;
	uipcClient_t clnfInfo;
	fd_set rfds;
	struct timeval tv;

	if((svrFileName==NULL)||(pData==NULL)||(pData->length>UIPC_DATA_MAX_SIZE)){
		return(-1);
	}
	memset((char *)&clnfInfo,0,sizeof(uipcClient_t));
	if(uipc_clnt_init_sock(svrFileName,&clnfInfo)){
		return(-1);
	}
	if(sendto(clnfInfo.sock,(char *)pData,sizeof(int) + pData->length,0,(struct sockaddr *)&clnfInfo.serverAddr,sizeof(struct sockaddr_un)) <= 0){
		rv = -1;
		goto Ret;
	}

	if(waitTime<0){
		nonBlockFlag = 0;
	}else{
		if(waitTime==0)/*at least 1 second*/
			waitTime = 1;
		tv.tv_sec = waitTime;
		tv.tv_usec = 0;
		FD_ZERO(&rfds);
		FD_SET(clnfInfo.sock,&rfds);

		rLen = select(clnfInfo.sock+1,&rfds,NULL,NULL,&tv);
		if(rLen==-1){
			rv = -1;
			goto Ret;
		}
		if(!FD_ISSET(clnfInfo.sock,&rfds)){
			rv = -1;
			goto Ret;
		}
	}
	ioctl(clnfInfo.sock,FIONBIO,&nonBlockFlag);
	peerLen = sizeof(struct sockaddr_un);
	if(recvfrom(clnfInfo.sock,(char *)pData,sizeof(uIpcData_t),0,(struct sockaddr *)&clnfInfo.serverAddr,&peerLen)<=0){
		rv = -1;
		goto Ret;
	}

Ret:
	uipc_clnt_release_resource(&clnfInfo);
	return(rv);
}


