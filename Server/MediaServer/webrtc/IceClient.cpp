/*
 * IceClient.cpp
 *
 *  Created on: 2019/06/28
 *      Author: max
 *		Email: Kingsleyyau@gmail.com
 */

#include "IceClient.h"

#include <include/CommonHeader.h>

// Common
#include <common/LogManager.h>
#include <common/Math.h>
#include <common/CommonFunc.h>
#include <common/Arithmetic.h>

// Crypto
#include <crypto/Crypto.h>

// libnice
#include <agent.h>
#include <debug.h>

#define INVALID_STREAMID 0

namespace mediaserver {
//::GMainContext *gWorkerContext = NULL;
//::GMainLoop* gWorkerLoop = NULL;
//::GThread* gWorkerLoopThread = NULL;

GMainLoop* gDeamonLoop = NULL;
GThread* gDeamonLoopThread = NULL;
GThread* gEpollThread = NULL;

KMutex gWorkerLoopMutex(KMutex::MutexType_Recursive);
long gWorkerLoopSize = 0;
long gWorkerLoopIndex = 0;
GMainContext** gWorkerContext = NULL;
GMainLoop** gWorkerLoop = NULL;
GThread** gWorkerLoopThread = NULL;

static const char *CandidateTypeName[] = {"host", "srflx", "prflx", "relay"};
static const char *CandidateTransportName[] = {"", "tcptype active", "tcptype passive", ""};

void* epoll_thread(void *data) {
	nice_epoll_run(-1);
	return 0;
}

void* loop_thread(void *data) {
	GMainLoop* pLoop = (GMainLoop *)data;
    g_main_loop_run(pLoop);

    return 0;
}

void* niceLogFunc(const char *logBuffer) {
	LogAync(
			LOG_INFO,
			"IceClient::niceLogFunc( "
			"[libnice], "
			"%s "
			")",
			logBuffer
			);
	return 0;
}

static string gStunServerIp = "";
static string gLocalIp = "";
static bool gbUseSecret = false;
static string gTurnUserName = "";
static string gTurnPassword = "";
static string gTurnShareSecret = "";
bool IceClient::GobalInit(const string& stunServerIp, const string& localIp, bool useShareSecret, const string& turnUserName, const string& turnPassword, const string& turnShareSecret) {
	bool bFlag = true;

	gStunServerIp = stunServerIp;
	gLocalIp = localIp;
	gbUseSecret = useShareSecret;
	gTurnUserName = turnUserName;
	gTurnPassword = turnPassword;
	gTurnShareSecret = turnShareSecret;

//	nice_debug_enable(TRUE);
//	nice_debug_verbose_enable();
	nice_debug_set_func((NICE_LOG_FUNC_IMP)&niceLogFunc);

	g_networking_init();
//	g_thread_init(NULL);
//	gWorkerContext = g_main_context_new();
	gDeamonLoop = g_main_loop_new(NULL, FALSE);
	gDeamonLoopThread = g_thread_new("IceDeamon", &loop_thread, gDeamonLoop);
	gEpollThread = g_thread_new("IceEpoll", &epoll_thread, NULL);

	gWorkerLoopSize = sysconf(_SC_NPROCESSORS_ONLN);
	gWorkerContext = new GMainContext*[gWorkerLoopSize];
	gWorkerLoop = new GMainLoop*[gWorkerLoopSize];
	gWorkerLoopThread = new GThread*[gWorkerLoopSize];
	for (int i = 0; i < gWorkerLoopSize; i++) {
		gWorkerContext[i] = g_main_context_new();
		gWorkerLoop[i] = g_main_loop_new(gWorkerContext[i], FALSE);
		gWorkerLoopThread[i] = g_thread_new("IceWorker", &loop_thread, gWorkerLoop[i]);
	}

	return bFlag;
}

//void IceClient::GobalStop() {
//}

void cb_closed(::GObject *src, ::GAsyncResult *res, ::gpointer user_data) {
//void cb_closed(void *src, void *res, void *data) {
	LogAync(
			LOG_INFO,
			"IceClient::cb_closed( "
			"this : %p, "
			"agent : %p "
			")",
			user_data,
			src
			);
	IceClient *pClient = (IceClient *)user_data;
	NiceAgent *agent = NICE_AGENT((GObject *)src);
	pClient->OnClose(agent);
}

void cb_nice_recv(NiceAgent *agent, guint streamId, guint componentId, guint len, gchar *buf, gpointer data) {
	IceClient* pClient = (IceClient *)data;
	pClient->OnNiceRecv(agent, streamId, componentId, len, buf);
}

void cb_candidate_gathering_done(NiceAgent *agent, guint streamId, gpointer data) {
	IceClient* pClient = (IceClient *)data;
	pClient->OnCandidateGatheringDone(agent, streamId);
}

void cb_component_state_changed(NiceAgent *agent, guint streamId, guint componentId, guint state, gpointer data) {
	IceClient* pClient = (IceClient *)data;
	pClient->OnComponentStateChanged(agent, streamId, componentId, state);
}

void cb_new_selected_pair_full(NiceAgent* agent, guint streamId, guint componentId, NiceCandidate *lcandidate, NiceCandidate* rcandidate, gpointer data) {
	IceClient* pClient = (IceClient *)data;
	pClient->OnNewSelectedPairFull(agent, streamId, componentId, lcandidate, rcandidate);
}

void cb_stream_removed_actually(NiceAgent *agent, guint streamId, gpointer data) {
	IceClient* pClient = (IceClient *)data;
	pClient->OnStreamRemovedActually(agent, streamId);
}

IceClient::IceClient() :
		mClientMutex(KMutex::MutexType_Recursive),
		mParamMutex(KMutex::MutexType_Recursive) {
	// TODO Auto-generated constructor stub
	// Status
	mRunning = false;
	mIceGatheringDone = false;

	// libnice
	mpAgent = NULL;
	mStreamId = INVALID_STREAMID;
	mComponentId = -1;
	mLastErrorCode = RequestErrorType_None;
	mbControlling = false;

	mSdp = "";
	mpIceClientCallback = NULL;
}

IceClient::~IceClient() {
	// TODO Auto-generated destructor stub
	Stop();
}

bool IceClient::Start(bool bControlling, const string name) {
	bool bFlag = false;

	LogAync(
			LOG_INFO,
			"IceClient::Start( "
			"this : %p "
			")",
			this
			);

	mClientMutex.lock();
	if( mRunning ) {
		Stop();
	}

	mLastErrorCode = RequestErrorType_None;
	mRunning = true;
	mbControlling = bControlling;

	gWorkerLoopMutex.lock();
	::GMainLoop* loop = gWorkerLoop[gWorkerLoopIndex];
	gWorkerLoopIndex++;
	gWorkerLoopIndex %= gWorkerLoopSize;
	gWorkerLoopMutex.unlock();

	mpAgent = nice_agent_new(g_main_loop_get_context(loop), NICE_COMPATIBILITY_RFC5245);

    /**
     * https://nice.freedesktop.org/libnice/NiceAgent.html
     *
     * stun-max-retransmissions:
     * 尝试发送Bind Request的次数
     * 发送CREATE_PERMISSION后, 尝试发送Send Indication的次数, 默认为7次, 大概为25s, (2^7*200/1000)=25s
     * 每次发送Send Indication的时间间隔会翻倍, 默认初始为200ms
     *
     * The maximum number of retransmissions of the STUN binding requests used in the gathering stage,
     * to find our local candidates, and used in the connection check stage,
     * to test the validity of each constructed pair.
     * This property is described as 'Rc' in the RFC 5389, with a default value of 7.
     * The timeout of each STUN request is doubled for each retransmission,
     * so the choice of this value has a direct impact on the time needed to move from the CONNECTED state to the READY state,
     * and on the time needed to complete the GATHERING state.
     *
     * stun-initial-timeout:
     * The initial timeout (msecs) of the STUN binding requests used in the gathering stage,
     * to find our local candidates. This property is described as 'RTO' in the RFC 5389 and RFC 5245.
     * This timeout is doubled for each retransmission, until “stun-max-retransmissions” have been done,
     * with an exception for the last restransmission,
     * where the timeout is divided by two instead (RFC 5389 indicates that a customisable multiplier 'Rm' to 'RTO' should be used).
     *
     */
	// 被动呼叫, controlling-mode为0
    g_object_set(mpAgent, "controlling-mode", bControlling?1:0, NULL);
    // TCP的Bind Request超时
//    g_object_set(mpAgent, "stun-reliable-timeout", 20000, NULL);
    g_object_set(mpAgent, "stun-max-retransmissions", 8, NULL);
    // NAT网关不支持UPNP, 禁用
    g_object_set(mpAgent, "upnp", FALSE,  NULL);
    // 使用tcp
    g_object_set(mpAgent, "ice-tcp", TRUE, NULL);
	// 强制使用turn转发
    g_object_set(mpAgent, "force-relay", TRUE, NULL);
    // 保持心跳
//    g_object_set(mpAgent, "keepalive-conncheck", TRUE, NULL);

    // 设置STUN服务器地址
    g_object_set(mpAgent, "stun-server", gStunServerIp.c_str(), NULL);
    g_object_set(mpAgent, "stun-server-port", 3478, NULL);

    if ( gLocalIp.length() > 0 ) {
		// 绑定本地IP
		NiceAddress addrLocal;
		nice_address_init(&addrLocal);
		nice_address_set_from_string(&addrLocal, gLocalIp.c_str());
		nice_agent_add_local_address(mpAgent, &addrLocal);
    }

    g_signal_connect(mpAgent, "candidate-gathering-done", G_CALLBACK(cb_candidate_gathering_done), this);
    g_signal_connect(mpAgent, "component-state-changed", G_CALLBACK(cb_component_state_changed), this);
    g_signal_connect(mpAgent, "new-selected-pair-full", G_CALLBACK(cb_new_selected_pair_full), this);
    g_signal_connect(mpAgent, "stream-removed-actually", G_CALLBACK(cb_stream_removed_actually), this);

	string username;
	string password;

    guint componentId = 1;
	guint streamId = nice_agent_add_stream(mpAgent, componentId);
	if ( streamId != 0 ) {
		mStreamId = streamId;
		mComponentId = componentId;

		if ( gbUseSecret ) {
			// 这里只需要精确到秒
			char user[256] = {0};
			time_t timer = time(NULL);
			snprintf(user, sizeof(user) - 1, "%lu:%s", timer + 3600, name.c_str());
			unsigned char sha1Pwd[EVP_MAX_MD_SIZE + 1] = {0};
			int length = Crypto::Sha1(gTurnShareSecret, user, sha1Pwd);
			Arithmetic art;
			string base64 = art.Base64Encode((const char *)sha1Pwd, length);

			username = user;
			password = base64;
		} else {
			username = gTurnUserName;
			password = gTurnPassword;
		}

		bFlag = true;
	    // 使用tcp
	    bFlag &= nice_agent_set_relay_info(mpAgent, streamId, componentId, gStunServerIp.c_str(), 3478, username.c_str(), password.c_str(), NICE_RELAY_TYPE_TURN_TCP);
//	    // 使用udp
//		bFlag &= nice_agent_set_relay_info(mpAgent, streamId, componentId, gStunServerIp.c_str(), 3478, username.c_str(), password.c_str(), NICE_RELAY_TYPE_TURN_UDP);
	    // 设置转发认证
	    bFlag &= nice_agent_set_stream_name(mpAgent, streamId, "video");
	    bFlag &= nice_agent_attach_recv(mpAgent, streamId, componentId, g_main_loop_get_context(loop), cb_nice_recv, this);
	    bFlag &= nice_agent_gather_candidates(mpAgent, streamId);
	}

	LogAync(
			LOG_INFO,
			"IceClient::Start( "
			"this : %p, "
			"[%s], "
			"agent : %p, "
			"streamId : %u, "
			"componentId : %u, "
			"username : %s, "
			"password : %s "
			")",
			this,
			FLAG_2_STRING(bFlag),
			mpAgent,
			streamId,
			componentId,
			username.c_str(),
			password.c_str()
			);

	mClientMutex.unlock();

	return bFlag;
}

void IceClient::Stop() {
	mClientMutex.lock();
	if( mRunning ) {
		::NiceAgent *agent = mpAgent;

		LogAync(
				LOG_INFO,
				"IceClient::Stop( "
				"this : %p, "
				"agent : %p "
				")",
				this,
				agent
				);

		if (mpAgent) {
			LogAync(
					LOG_INFO,
					"IceClient::Stop( "
					"this : %p, "
					"[Request Turnserver Close Port], "
					"agent : %p "
					")",
					this,
					agent
					);
			mRunning = false;
			/**
			 * 通知turnserver关闭对应端口
			 * 不能调用nice_agent_remove_stream, 否则会导致有些端口关不掉
			 * 问题: 由于nice_agent_remove_stream对pop出来stream的关闭操作是通过timer异步进行, 所以可能导致执行timer回调时候agent已经为空, 从而不能触发回调处理
			 * 	1.调用nice_agent_remove_stream, 放入异步队列进行关闭, 但是回调时候agent(弱引用)可能已经释放, 导致不能执行关闭端口的回调函数
			 * 		refresh_prune_stream_async->agent_timeout_add_with_context_internal->timeout_cb {
			 * 			...
			 * 			TimeoutData *data = user_data;
			 * 			NiceAgent *agent;
			 * 		  	agent = g_weak_ref_get (&data->agent_ref);
			 * 		  	if (agent == NULL) {
			 * 		  		return G_SOURCE_REMOVE;
			 * 		  	}
			 * 		 	...
			 * 		 	ret = data->function (agent, data->user_data);
			 * 		 	...
			 * 		}->on_refresh_removed
			 * 	2.调用g_object_unref, 直接释放
			 * 		g_object_unref->nice_agent_dispose -> {
			 * 			...
			 * 			while (agent->streams) {
			 * 				nice_stream_close (agent, s);
			 * 			}
			 * 			...
			 * 		}
			 */
			// Notice Turn Server To Remove Port
			mCloseCond.lock();
			nice_agent_close_async(mpAgent, (GAsyncReadyCallback)cb_closed, this);
			mCloseCond.wait();
			mCloseCond.unlock();

			// Remove Stream
			LogAync(
					LOG_INFO,
					"IceClient::Stop( "
					"this : %p, "
					"[Remove Local Stream], "
					"agent : %p, "
					"mStreamId : %u "
					")",
					this,
					agent,
					mStreamId
					);
			if ( mStreamId != INVALID_STREAMID ) {
				mStreamRemoveCond.lock();
				nice_agent_remove_stream(mpAgent, mStreamId);
				mStreamRemoveCond.wait();
				mStreamRemoveCond.unlock();
			}

			// Release Agent
			LogAync(
					LOG_INFO,
					"IceClient::Stop( "
					"this : %p, "
					"[Release Agent], "
					"agent : %p "
					")",
					this,
					agent
					);
			g_signal_handlers_disconnect_by_data(mpAgent, this);
			g_object_unref(mpAgent);

			// Reset Parameter
			mpAgent = NULL;
			mStreamId = INVALID_STREAMID;
			mComponentId = -1;

		} else {
			mRunning = false;
		}

		mIceGatheringDone = false;

		LogAync(
				LOG_INFO,
				"IceClient::Stop( "
				"this : %p, "
				"[OK], "
				"agent : %p "
				")",
				this,
				agent
				);
	}
	mClientMutex.unlock();
}

void IceClient::SetCallback(IceClientCallback *callback) {
	mpIceClientCallback = callback;
}

void IceClient::SetRemoteSdp(const string& sdp) {
	mClientMutex.lock();

	mSdp = sdp;

	if ( mpAgent ) {
		NiceComponentState state = nice_agent_get_component_state(mpAgent, mStreamId, mComponentId);
		LogAync(
				LOG_INFO,
				"IceClient::SetRemoteSdp( "
				"this : %p, "
				"mIceGatheringDone : %s, "
				"state : %s "
				")",
				this,
				FLAG_2_STRING(mIceGatheringDone),
				nice_component_state_to_string((NiceComponentState)state)
				);

//		if( state == NICE_COMPONENT_STATE_CONNECTING ) {
		mParamMutex.lock();
		if ( mIceGatheringDone && state < NICE_COMPONENT_STATE_CONNECTED ) {
			ParseRemoteSdp(mStreamId);
		}
		mParamMutex.unlock();
	}

	mClientMutex.unlock();
}

int IceClient::SendData(const void *data, unsigned int len) {
	int sendSize = 0;
	sendSize = nice_agent_send(mpAgent, mStreamId, mComponentId, len, (const gchar *)data);
	return sendSize;
}

const string& IceClient::GetLocalAddress() {
	return mLocalAddress;
}

const string& IceClient::GetRemoteAddress() {
	return mRemoteAddress;
}

bool IceClient::IsConnected() {
	bool bFlag = false;
	if ( mpAgent ) {
		NiceComponentState state = nice_agent_get_component_state(mpAgent, mStreamId, mComponentId);
		if ( state == NICE_COMPONENT_STATE_CONNECTED || state == NICE_COMPONENT_STATE_READY ) {
			bFlag = true;
		}
	}
	return bFlag;
}

bool IceClient::ParseRemoteSdp(unsigned int streamId) {
	bool bFlag = false;

	LogAync(
			LOG_INFO,
			"IceClient::ParseRemoteSdp( "
			"this : %p, "
			"sdp :\n%s"
			")",
			this,
			mSdp.c_str()
			);

    gchar *ufrag = NULL;
    gchar *pwd = NULL;
    GSList *plist = nice_agent_parse_remote_stream_sdp(mpAgent, streamId, mSdp.c_str(), &ufrag, &pwd);
    if ( ufrag && pwd && g_slist_length(plist) > 0 ) {
		LogAync(
				LOG_INFO,
				"IceClient::ParseRemoteSdp( "
				"this : %p, "
				"plist_count : %d, "
				"ufrag : %s, "
				"pwd : %s "
				")",
				this,
				g_slist_length(plist),
				ufrag,
				pwd
				);

//		char candStr[1024] = {'0'};
//	    gchar ip[INET6_ADDRSTRLEN] = {0};
//	    gchar baseip[INET6_ADDRSTRLEN] = {0};
//		for(int i = 0; i < g_slist_length(plist); i++) {
//			NiceCandidate *remote = (NiceCandidate *)g_slist_nth(plist, i)->data;
//			nice_address_to_string(&remote->addr, ip);
//			nice_address_to_string(&remote->base_addr, baseip);
//
//			if ( remote->type == NICE_CANDIDATE_TYPE_RELAYED ) {
//				snprintf(candStr, sizeof(candStr) - 1,
//						"a=candidate:%s 1 %s %u %s %u typ %s raddr %s rport 9\n",
//						remote->foundation,
//						(remote->transport == NICE_CANDIDATE_TRANSPORT_UDP)?"udp":"tcp",
//						remote->priority,
//						ip,
//						nice_address_get_port(&remote->addr),
//						CandidateTypeName[remote->type],
//						baseip
//						);
//
//			} else {
//				snprintf(candStr, sizeof(candStr) - 1,
//						"a=candidate:%s 1 %s %u %s %u typ %s %s\n",
//						remote->foundation,
//						(remote->transport == NICE_CANDIDATE_TRANSPORT_UDP)?"udp":"tcp",
//						remote->priority,
//						ip,
//						nice_address_get_port(&remote->addr),
//						CandidateTypeName[remote->type],
//						CandidateTransportName[remote->transport]
//						);
//			}
//    		LogAync(
//    				LOG_WARNING,
//    				"IceClient::ParseRemoteSdp( "
//    				"this : %p, "
//    				"candStr : %s"
//    				")",
//    				this,
//					candStr
//    				);
//		}

        bFlag = nice_agent_set_remote_credentials(mpAgent, streamId, ufrag, pwd);
        if( !bFlag ) {
    		LogAync(
    				LOG_WARNING,
    				"IceClient::ParseRemoteSdp( "
    				"this : %p, "
    				"[nice_agent_set_remote_credentials Fail], "
    				"ufrag : %s, "
    				"pwd : %s "
    				")",
    				this,
    				ufrag,
    				pwd
    				);
        }

        if( bFlag ) {
        	bFlag = (nice_agent_set_remote_candidates(mpAgent, streamId, mComponentId, plist) >= 1);
        }

        if( !bFlag ) {
    		LogAync(
    				LOG_WARNING,
    				"IceClient::ParseRemoteSdp( "
    				"this : %p, "
    				"[nice_agent_set_remote_candidates Fail], "
    				"ufrag : %s, "
    				"pwd : %s "
    				")",
    				this,
    				ufrag,
    				pwd
    				);
        }
    } else {
		LogAync(
				LOG_WARNING,
				"IceClient::ParseRemoteSdp( "
				"this : %p, "
				"[nice_agent_parse_remote_stream_sdp Fail], "
				"sdp :\n%s"
				")",
				this,
				mSdp.c_str()
				);
    }

//	LogAync(
//			LOG_DEBUG,
//			"IceClient::ParseRemoteSdp( "
//			"this : %p, "
//			"ufrag : %s, "
//			"pwd : %s, "
//			"remoteSdp : %s"
//			")",
//			this,
//			ufrag,
//			pwd,
//			mSdp.c_str()
//			);

    if ( ufrag ) {
    	g_free(ufrag);
    }
    if ( pwd ) {
    	g_free(pwd);
    }

    if (plist) {
    	g_slist_free(plist);
    }

    return bFlag;
}

void IceClient::OnClose(::NiceAgent *agent) {
	LogAync(
			LOG_INFO,
			"IceClient::OnClose( "
			"this : %p, "
			"agent : %p "
			")",
			this,
			agent
			);

	mCloseCond.lock();
//	if( mRunning ) {
		if( mpIceClientCallback ) {
			mpIceClientCallback->OnIceClose(this);
		}
//	}
	mCloseCond.signal();
	mCloseCond.unlock();

	LogAync(
			LOG_INFO,
			"IceClient::OnClose( "
			"this : %p, "
			"[Exit], "
			"agent : %p "
			")",
			this,
			agent
			);
}

void IceClient::OnNiceRecv(::NiceAgent *agent, unsigned int streamId, unsigned int componentId, unsigned int len, char *buf) {
//	LogAync(
//			LOG_INFO,
//			"IceClient::OnNiceRecv( "
//			"this : %p, "
//			"streamId : %u, "
//			"componentId : %u, "
//			"len : %u "
//			")",
//			this,
//			streamId,
//			componentId,
//			len
//			);

	if ( mpIceClientCallback ) {
		mpIceClientCallback->OnIceRecvData(this, (const char *)buf, len, streamId, componentId);
	}
}

void IceClient::OnCandidateGatheringDone(::NiceAgent *agent, unsigned int streamId) {
	gchar *localCandidate = nice_agent_generate_local_sdp(agent);
	LogAync(
			LOG_INFO,
			"IceClient::OnCandidateGatheringDone( "
			"this : %p, "
			"streamId : %u, "
			"localCandidate :\n%s"
			")",
			this,
			streamId,
			localCandidate
			);

    gchar *ufrag = NULL;
    gchar *pwd = NULL;
    gchar ip[INET6_ADDRSTRLEN] = {0};
    gchar baseip[INET6_ADDRSTRLEN] = {0};

    mParamMutex.lock();
	mIceGatheringDone = true;
	mParamMutex.unlock();

    bool bFlag = true;
	if ( bFlag ) {
	    bFlag = nice_agent_get_local_credentials(agent, streamId, &ufrag, &pwd);
	}
	if ( bFlag ) {
		bFlag = false;
		vector<string> candArray;
		string ipUse;
		unsigned int portUse = 0;
		unsigned int priority = 0xFFFFFFFF;

		char candStr[1024] = {'0'};
		GSList *cands = nice_agent_get_local_candidates(agent, streamId, 1);
		if( cands ) {
			int count = g_slist_length(cands);
			for(int i = 0; i < count; i++) {
				NiceCandidate *local = (NiceCandidate *)g_slist_nth(cands, i)->data;
				nice_address_to_string(&local->addr, ip);
				unsigned int localPort = (nice_address_get_port(&local->addr)==0)?9:nice_address_get_port(&local->addr);
				nice_address_to_string(&local->base_addr, baseip);
				unsigned int basePort = (nice_address_get_port(&local->base_addr)==0)?9:nice_address_get_port(&local->base_addr);

				if ( priority > local->priority ) {
					priority = local->priority;
					ipUse = ip;
					portUse = nice_address_get_port(&local->addr);
				}

				string transport;
					if ( strlen(CandidateTransportName[local->transport]) > 0 ) {
						transport += " ";
						transport += CandidateTransportName[local->transport];
					}

				if ( local->type == NICE_CANDIDATE_TYPE_RELAYED || local->type == NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE ) {
					snprintf(candStr, sizeof(candStr) - 1,
							"a=candidate:%s 1 %s %u %s %u typ %s raddr %s rport %u%s\n",
							local->foundation,
							(local->transport == NICE_CANDIDATE_TRANSPORT_UDP)?"UDP":"TCP",
							local->priority,
							ip,
							localPort,
							CandidateTypeName[local->type],
							baseip,
							basePort,
						    transport.c_str()
							);
					priority = local->priority;
					ipUse = ip;
					portUse = nice_address_get_port(&local->addr);

				} else {
					snprintf(candStr, sizeof(candStr) - 1,
							"a=candidate:%s 1 %s %u %s %u typ %s%s\n",
							local->foundation,
							(local->transport == NICE_CANDIDATE_TRANSPORT_UDP)?"UDP":"TCP",
							local->priority,
							ip,
							localPort,
							CandidateTypeName[local->type],
							transport.c_str()
							);
				}

				candArray.push_back(string(candStr));
			}

			if( mpIceClientCallback ) {
				mpIceClientCallback->OnIceCandidateGatheringDone(this, ipUse, portUse, candArray, ufrag, pwd);
			}

			bFlag = true;
			if (!mbControlling) {
				bFlag = ParseRemoteSdp(streamId);
			}
		}
	}

	if ( !bFlag ) {
		LogAync(
				LOG_WARNING,
				"IceClient::OnCandidateGatheringDone( "
				"this : %p, "
				"[CandidateGathering Fail], "
				"streamId : %u, "
				"localCandidate :\n%s"
				")",
				this,
				streamId,
				localCandidate
				);
		if( mpIceClientCallback ) {
			mpIceClientCallback->OnIceCandidateGatheringFail(this, RequestErrorType_WebRTC_No_Server_Candidate_Info_Found_Fail);
		}
	}

	if ( ufrag ) {
		g_free(ufrag);
	}
	if ( pwd ) {
		g_free(pwd);
	}
	if ( localCandidate ) {
		g_free(localCandidate);
	}
}

void IceClient::OnComponentStateChanged(::NiceAgent *agent, unsigned int streamId, unsigned int componentId, unsigned int state) {
	LogAync(
			LOG_INFO,
			"IceClient::OnComponentStateChanged( "
			"this : %p, "
			"streamId : %u, "
			"componentId : %u, "
			"state : %s "
			")",
			this,
			streamId,
			componentId,
			nice_component_state_to_string((NiceComponentState)state)
			);

	if (state == NICE_COMPONENT_STATE_CONNECTED) {
		if( mpIceClientCallback ) {
			mpIceClientCallback->OnIceConnected(this);
		}
	} else if (state == NICE_COMPONENT_STATE_FAILED) {
		if( mpIceClientCallback ) {
			mpIceClientCallback->OnIceFail(this);
		}
	}
}

void IceClient::OnNewSelectedPairFull(::NiceAgent* agent, unsigned int streamId, unsigned int componentId, ::NiceCandidate *local, ::NiceCandidate* remote) {
	gchar localIp[INET6_ADDRSTRLEN] = {0};
	nice_address_to_string(&local->addr, localIp);
	gchar remoteIp[INET6_ADDRSTRLEN] = {0};
	nice_address_to_string(&remote->addr, remoteIp);

	char tmp[128];
	sprintf(tmp, "(%s)(%s)%s:%u", (local->transport==NICE_CANDIDATE_TRANSPORT_UDP)?"udp":"tcp", CandidateTypeName[local->type], localIp, nice_address_get_port(&local->addr));
	mLocalAddress = tmp;
	sprintf(tmp, "(%s)(%s)%s:%u", (remote->transport==NICE_CANDIDATE_TRANSPORT_UDP)?"udp":"tcp", CandidateTypeName[remote->type], remoteIp, nice_address_get_port(&remote->addr));
	mRemoteAddress = tmp;

	char *localSdp = nice_agent_generate_local_sdp(mpAgent);
	LogAync(
			LOG_INFO,
			"IceClient::OnNewSelectedPairFull( "
			"this : %p, "
			"streamId : %u, "
			"componentId : %u, "
			"local : %s, "
			"remote : %s "
			")",
			this,
			streamId,
			componentId,
			mLocalAddress.c_str(),
			mRemoteAddress.c_str()
			);

	if( mpIceClientCallback ) {
		mpIceClientCallback->OnIceNewSelectedPairFull(this);
	}
}

void IceClient::OnStreamRemovedActually(::NiceAgent *agent, unsigned int streamId) {
	LogAync(
			LOG_INFO,
			"IceClient::OnStreamRemovedActually( "
			"this : %p, "
			"agent : %p, "
			"streamId : %u "
			")",
			this,
			agent,
			streamId
			);

	mStreamRemoveCond.lock();
	mStreamRemoveCond.signal();
	mStreamRemoveCond.unlock();

	LogAync(
			LOG_INFO,
			"IceClient::OnStreamRemovedActually( "
			"this : %p, "
			"[Exit], "
			"agent : %p, "
			"streamId : %u "
			")",
			this,
			agent,
			streamId
			);
}

} /* namespace mediaserver */
