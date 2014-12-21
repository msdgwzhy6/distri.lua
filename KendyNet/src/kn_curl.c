#include "kendynet_private.h"
#include "kn_type.h"
#include "kn_dlist.h"
#include "kn_timer.h"
#include "kn_curl.h"

struct kn_CURLM{
	kn_dlist curls;
	CURLM   *c_handle;
	kn_timer_t timer;
	engine_t e;
};

struct kn_CURL{
	kn_dlist_node node;
	kn_CURLM_t    c_handle;
	void (*cb)(kn_CURL_t,CURLMsg*,void*);
	CURL *curl;
	void *ud;
};

typedef struct curl_conn{
	handle                comm_head;
	kn_CURLM_t            c_handle;          
	int                   events;
}curl_conn,*curl_conn_t;

static void curl_on_active(handle_t s,int event){
  int running_handles;
  int flags = 0;
  curl_conn_t conn;
  CURLMsg *message;
  int pending;

  if ((event & (EPOLLERR | EPOLLHUP)) || (event & (EPOLLRDHUP | EPOLLIN)))
    flags |= CURL_CSELECT_IN;
  if (event & EPOLLOUT)
    flags |= CURL_CSELECT_OUT;

  conn = (curl_conn_t)s;
  if(conn->c_handle->timer){
	  kn_del_timer(conn->c_handle->timer);
	  conn->c_handle->timer = NULL; 
  }
  curl_multi_socket_action(kn_CURLM_get(conn->c_handle), ((handle_t)conn)->fd, flags,&running_handles);
  while ((message = curl_multi_info_read(kn_CURLM_get(conn->c_handle), &pending))) {
		kn_CURL_t curl;
		curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &curl);
		if(message->msg == CURLMSG_DONE){
			if(curl->cb)
				curl->cb(curl,message,curl->ud);
			else
				kn_curl_easy_cleanup(curl);
		}else{
		    fprintf(stderr, "CURLMSG default\n");
			abort();
		}
   }
}

static curl_conn_t create_curl_conn(curl_socket_t s,kn_CURLM_t cm){
	curl_conn_t conn = calloc(1,sizeof(*conn));
	conn->comm_head.fd = s;
	conn->comm_head.on_events = curl_on_active;
	conn->c_handle = cm;
	return conn;
}

static int curl_conn_add_read(engine_t e,curl_conn_t conn){
	int events = conn->events | EPOLLIN | EPOLLRDHUP;
	int ret;
	if(conn->events == 0)
		ret = kn_event_add(e,(handle_t)conn,events);
	else
		ret = kn_event_mod(e,(handle_t)conn,events);
		
	if(ret == 0) conn->events = events;
	return ret;	
}

static int curl_conn_add_write(engine_t e,curl_conn_t conn){
	int events = conn->events | EPOLLOUT;
	int ret;
	if(conn->events == 0)
		ret = kn_event_add(e,(handle_t)conn,events);
	else
		ret = kn_event_mod(e,(handle_t)conn,events);
		
	if(ret == 0) conn->events = events;
	return ret;		
}

static int handle_socket(CURL *easy, curl_socket_t s, int action, void *userp,void *socketp)
{
  kn_CURL_t curl = NULL;
  kn_CURLM_t curlm = NULL;
  curl_conn_t conn = NULL;
  curl_easy_getinfo(easy,CURLINFO_PRIVATE,&curl);
  curlm = curl->c_handle;	
  if (action == CURL_POLL_IN || action == CURL_POLL_OUT) {
    	if (socketp) {
      		conn = (curl_conn_t) socketp;
    	}else {
      		conn = create_curl_conn(s,curlm);
    	}
    	curl_multi_assign(curlm->c_handle, s,(void *) conn);
  }

  switch (action) {
  case CURL_POLL_IN:
	   curl_conn_add_read(curlm->e,conn);
    break;
  case CURL_POLL_OUT:
		curl_conn_add_write(curlm->e,conn);
    break;
  case CURL_POLL_REMOVE:
    if (socketp) {
		conn = (curl_conn_t) socketp;
		kn_event_del(curlm->e,(handle_t)conn);
		free(conn); 
		curl_multi_assign(curlm->c_handle,s,NULL);
    }
    break;
  default:
    abort();
  }

  return 0;
}

static pthread_once_t g_curl_key_once = PTHREAD_ONCE_INIT;
static void curl_once_routine(){
	if(curl_global_init(CURL_GLOBAL_ALL)){
		fprintf(stderr, "Could not init cURL\n");		
		abort();
	}
}

static int timer_callback(kn_timer_t timer){
	kn_CURLM_t cm = (kn_CURLM_t)kn_timer_getud(timer);
	int running_handles;	
	curl_multi_socket_action(cm->c_handle, CURL_SOCKET_TIMEOUT,0,&running_handles);
	//cm->timer = NULL;//不需要销毁,callback返回0后会被自动销毁
	return 1;
}

/*void start_timeout(CURLM *multi, long timeout_ms, void *userp)
{
  if(timeout_ms <= 0)
    timeout_ms = 1; 
  kn_CURLM_t cm = (kn_CURLM_t)userp;	
  if(!cm->timer) cm->timer = kn_reg_timer(cm->e,timeout_ms,timer_callback,cm);   
   //uv_timer_start(&timeout, on_timeout, timeout_ms, 0);
}*/



kn_CURLM_t kn_CURLM_init(engine_t e){
	pthread_once(&g_curl_key_once,curl_once_routine);
	CURLM *c = curl_multi_init();
	if(!c) return NULL;
	kn_CURLM_t cm = calloc(1,sizeof(*cm));
	cm->c_handle = c;
	cm->e = e;
	kn_dlist_init(&cm->curls);
	curl_multi_setopt(c, CURLMOPT_SOCKETFUNCTION, handle_socket);
    //curl_multi_setopt(c, CURLMOPT_TIMERDATA, cm);	
	//curl_multi_setopt(c, CURLMOPT_TIMERFUNCTION, start_timeout);	
	return cm;
}

CURLM* kn_CURLM_get(kn_CURLM_t cm){
	return cm->c_handle;
}

CURLMcode kn_CURLM_add(kn_CURLM_t cm,kn_CURL_t curl,void (*cb)(kn_CURL_t,CURLMsg *message,void*),void*ud){
	if(0 != kn_dlist_push(&cm->curls,(kn_dlist_node*)curl))
		return CURL_LAST;
	curl->cb = cb;
	curl->ud = ud;
	curl->c_handle = cm;
	CURLMcode code = curl_multi_add_handle(cm->c_handle,curl->curl);
	if(code != CURLM_OK) kn_dlist_remove((kn_dlist_node*)curl);
	else{
		curl_easy_setopt(curl->curl,CURLOPT_PRIVATE,curl);
		if(!cm->timer) cm->timer = kn_reg_timer(cm->e,1,timer_callback,cm);
	}
	return code;
}

void kn_CURLM_cleanup(kn_CURLM_t cm){
	if(cm->timer) kn_del_timer(cm->timer);
	kn_CURL_t curl;
	while((curl = (kn_CURL_t)kn_dlist_pop(&cm->curls)))
		kn_curl_easy_cleanup(curl);
	curl_multi_cleanup(cm->c_handle);	
	free(cm);
	return;
}

CURL* kn_curl_easy_get(kn_CURL_t curl){
	return curl->curl;
}

void kn_curl_easy_cleanup(kn_CURL_t curl){
	//printf("kn_curl_easy_cleanup\n");
	kn_dlist_remove((kn_dlist_node*)curl);
    curl_multi_remove_handle(kn_CURLM_get(curl->c_handle), curl->curl);
    curl_easy_cleanup(curl->curl);	
	free(curl);
}

kn_CURL_t kn_curl_easy_init(){
	CURL *curl = curl_easy_init();
	if(!curl) return NULL;
	kn_CURL_t c = calloc(1,sizeof(*c));
	c->curl = curl;
	return c;
}


