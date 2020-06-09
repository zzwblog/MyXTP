// testTradeApi.cpp : 定义控制台应用程序的入口点。
//#include "xtp_trader_api.h"
#include <string>
#include <map>
#include <iostream>
#include <cstring>
#include <deque>


#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif // _WIN32

#include "trade_spi.h"
#include "FileUtils.h"
#include "xtp_quote_api.h"
#include "quote_spi.h"
#include "xhost_md5.h"
#include "xtp_monitor_guest_api.h"

#include "Strategy.h"


XTP::API::TraderApi* pUserApi;
bool is_connected_ = false;
std::string trade_server_ip;
int trade_server_port;
std::string account_name;
std::string account_pw;
uint64_t session_id_ = 0;
std::map<uint64_t, uint64_t> map_session;
uint64_t* session_arrary = NULL;
FileUtils* fileUtils = NULL;
XTPOrderInsertInfo* orderList = NULL;
std::string quote_server_ip;
int quote_server_port;
std::string quote_username;
std::string quote_password;
int quote_protocol = 2;//原型XTP_PROTOCOL_TYPE quote_protocol = XTP_PROTOCOL_UDP
std::string host_monitor_username;
std::string host_monitor_password;

//策略是否运行状态的标识
bool strategy_status = false;

///***********************************
#ifdef WIN32
const char* configPath = "..\\..\\api\\config.json";
const std::string settingConfigPath = "..\\..\\api\\setting.json";
#else
const char* configPath = "config.json";
const std::string settingConfigPath = "setting.json";
#endif

XTP::API::QuoteApi* pQuoteApi;
Strategy* myStrategy = nullptr;
uint64_t globalSession_id = 0;
std::string monitorValue;
int quote_exchange;
double Eps;
int64_t updateTime = 0, timeWindow;//测速时间计算
int64_t SellThresholds;//卖策略阈值
std::mutex upperMut, lastMut, bookMut, ourMut, otherMut, tradeMut;//互斥锁
std::unordered_map<std::string, double> upper_price_map(MapInitSize);//监听涨停价
std::unordered_map<std::string, double> last_price_map(MapInitSize);//监听最新价，以最新价卖出
std::unordered_map<std::string, double> book_order_price(MapInitSize);//十档申第一档卖价为0时才允许跟单
std::unordered_map<std::string, std::deque<int64_t>> other_order_tickers(MapInitSize);//下单成功的触发的对方订单号
std::unordered_map<std::string, bool>has_trade_ticker(MapInitSize);//表示是否交易
std::unordered_map<std::string, int64_t>our_order_tickers(MapInitSize);//用来存储我们的委托订单

//*************************测速**********
bool bIsMeasure = false;
long long nMeasureThreshold = 999999999999;
XTPOrderInsertInfo measureInsertOrder;
///************************************
//监控客户登录时，验证账户有效性的回调函数
int32_t CheckLogin(const char* username, const char* password, const char* mac_add, const char* ip)
{
	std::cout << "CheckLogin. name:" << username << ", password:" << password << ",mac_add:" << mac_add << ",ip:" << ip << std::endl;

	//由于用户密码是经过MD5加密的，因此需要将您设定的监控账户密码加密后，与传入的密码进行校验
	char temp[1024] = { 0 };
	calc_md5(host_monitor_password.c_str(), strlen(host_monitor_password.c_str()), temp);//MD5加密
	std::cout << temp << std::endl;

	if ((strcmp(password, temp) == 0) && (strcmp(username, host_monitor_username.c_str()) == 0))
	{
		//用户名、密码相同，可以视为验证通过
		return 0;
	}

	return 10001; //错误code，用户自定义
}

//监控客户端启动策略
int32_t StrategyStart()
{
	std::cout << "StrategyStart." << std::endl;
	strategy_status = true;
	//监听数据	
	Eps = (double)(fileUtils->intForKey("eps")) / (1e8);//因为无法获取double数据，故配置中1为精度0.00000001
	pQuoteApi->SubscribeAllTickByTick((XTP_EXCHANGE_TYPE)quote_exchange);
	return 0;
}

//监控客户端停止策略
int32_t StrategyStop()
{
	std::cout << "StrategyStop." << std::endl;
	pQuoteApi->UnSubscribeAllTickByTick((XTP_EXCHANGE_TYPE)quote_exchange);
	pQuoteApi->UnSubscribeAllMarketData((XTP_EXCHANGE_TYPE)quote_exchange);
	pQuoteApi->UnSubscribeAllOrderBook((XTP_EXCHANGE_TYPE)quote_exchange);
	strategy_status = false;
	return 0;
}

//与monitor服务器断线回调函数
void MonitorDisconnected()
{
	std::cout << "Disconnected from monitor server." << std::endl;
}

int32_t ClientSetParameter(const char* key, const char* value)
{
	std::cout << "Client set --- Key:" << key << ", Value:" << value << std::endl;
	if (strcmp(key, "startMeasure") == 0)
	{
		bIsMeasure = true;
		char buffer[128];
		sprintf(buffer, "startMeasure: ticker:%s, price:%lf, qty:%d, threshold:%ld", measureInsertOrder.ticker, measureInsertOrder.price, measureInsertOrder.quantity, nMeasureThreshold);
		SendMsg(XTP_LOG_LEVEL_TRACE, "Measure", buffer);
	}
	else if (strcmp(key, "setMeasureTicker") == 0)
	{
		strcpy(measureInsertOrder.ticker, value);
	}
	else if (strcmp(key, "setMeasurePrice") == 0)//输入跌停价
	{
		measureInsertOrder.price = atof(value);
	}
	else if (strcmp(key, "setMeasureQty") == 0)
	{
		measureInsertOrder.quantity = atoi(value);
	}
	else if (strcmp(key, "setMeasureThreshold") == 0)
	{
		nMeasureThreshold = atoll(value);
	}
	else if (strcmp(key, "stopMeasure") == 0)
	{
		bIsMeasure = false;
	}
	else if (strcmp(key, "orderbook") == 0)
	{
		monitorValue = value;
	}
	return 0;
}

int main()
{
	fileUtils = new FileUtils();
	if (!fileUtils->init(configPath))
	{
		std::cout << "The config.json file parse error." << std::endl;
#ifdef _WIN32
		system("pause");
#endif
		return 0;
	}
	//读取策略配置
	timeWindow = fileUtils->intForKey("time_window");//时间窗口
	SellThresholds = fileUtils->intForKey("sell_thresholds");//卖策略

	//读取交易配置
	trade_server_ip = fileUtils->stdStringForKey("trade_ip");
	trade_server_port = fileUtils->intForKey("trade_port");
	int out_count = fileUtils->intForKey("out_count");
	bool auto_save = fileUtils->boolForKey("auto_save");
	int client_id = fileUtils->intForKey("client_id");
	int account_count = fileUtils->countForKey("account");
	int resume_type = fileUtils->intForKey("resume_type");
	std::string account_key = fileUtils->stdStringForKey("account_key");
#ifdef _WIN32
	std::string filepath = fileUtils->stdStringForKey("path");
#else
	std::string filepath = fileUtils->stdStringForKey("path_linux");
#endif // _WIN32

	//读取行情配置
	quote_server_ip = fileUtils->stdStringForKey("quote_ip");
	quote_server_port = fileUtils->intForKey("quote_port");
	quote_username = fileUtils->stdStringForKey("quote_user");
	quote_password = fileUtils->stdStringForKey("quote_password");
	quote_protocol = fileUtils->intForKey("quote_protocol");  //原型 = (XTP_PROTOCOL_TYPE)(fileUtils->intForKey("quote_protocol"));
	int32_t quote_buffer_size = fileUtils->intForKey("quote_buffer_size");

	//读取心跳超时配置
	int32_t heat_beat_interval = fileUtils->intForKey("hb_interval");

	//读取host guest配置
	std::string host_server_ip = fileUtils->stdStringForKey("host_ip");
	int32_t host_server_port = fileUtils->intForKey("host_port");
	std::string host_username = fileUtils->stdStringForKey("host_user");
	host_monitor_username = fileUtils->stdStringForKey("host_monitor_name");
	host_monitor_password = fileUtils->stdStringForKey("host_monitor_password");

	//注册Host的响应函数
	RegisterMonitorClientLoginFunc(&CheckLogin);
	RegisterStartFunc(&StrategyStart);
	RegisterStopFunc(&StrategyStop);
	RegisterDisconnectedFunc(&MonitorDisconnected);
	RegisterSetParameterFunc(&ClientSetParameter);
	//登录至Host服务器
	int32_t ret = ConnectToMonitor(host_server_ip.c_str(), host_server_port, host_username.c_str(), strategy_status);
	if (ret == 0)
	{
		//host guest user login success
		std::cout << host_username << " login to host success." << std::endl;
	}
	else
	{
		std::cout << host_username << " login to host failed." << std::endl;
#ifdef _WIN32
		system("pause");
#endif
		return 0;
	}

	//-----------------以下为监控客户端启动策略后的运行---------------------------------------------
	//初始化行情api
	pQuoteApi = XTP::API::QuoteApi::CreateQuoteApi(client_id, filepath.c_str());
	MyQuoteSpi* pQuoteSpi = new MyQuoteSpi();
	pQuoteApi->RegisterSpi(pQuoteSpi);

	//设定行情服务器超时时间，单位为秒
	pQuoteApi->SetHeartBeatInterval(heat_beat_interval); //此为1.1.16新增接口
	//设定行情本地缓存大小，单位为MB
	pQuoteApi->SetUDPBufferSize(quote_buffer_size);//此为1.1.16新增接口

	int loginResult_quote = -1;
	//登录行情服务器,自1.1.16开始，行情服务器支持UDP连接，推荐使用UDP                                                          //原型quote_protocol
	loginResult_quote = pQuoteApi->Login(quote_server_ip.c_str(), quote_server_port, quote_username.c_str(), quote_password.c_str(), (XTP_PROTOCOL_TYPE)quote_protocol);
	if (loginResult_quote == 0)
	{
		//登录成功，向监控端发送登录成功日志
		char strlog[1024] = { 0 };
		sprintf(strlog, "%s success to login quote %s:%d.", quote_username.c_str(), quote_server_ip.c_str(), quote_server_port);
		SendMsg(XTP_LOG_LEVEL_INFO, "Login quote", strlog);

	}
	else
	{
		//登录失败，获取失败原因
		XTPRI* error_info = pQuoteApi->GetApiLastError();
		std::cout << "Login to server error, " << error_info->error_id << " : " << error_info->error_msg << std::endl;

		//登录失败，向监控端发送登录失败日志
		char strlog[1024] = { 0 };
		sprintf(strlog, "%s failed to login quote %s:%d, error_id: %d, error_msg: %s.", quote_username.c_str(), quote_server_ip.c_str(), quote_server_port, error_info->error_id, error_info->error_msg);
		SendMsg(XTP_LOG_LEVEL_ERROR, "Login quote", strlog, 1);
	}


	if (account_count > 0)
	{
		//针对多用户的情况
		orderList = new XTPOrderInsertInfo[account_count];
	}

	//-----------------以下为监控客户端启动策略后的运行---------------------------------------------
	//初始化交易类Api
	pUserApi = XTP::API::TraderApi::CreateTraderApi(client_id, filepath.c_str());			// 创建UserApi
	pUserApi->SubscribePublicTopic((XTP_TE_RESUME_TYPE)resume_type);
	pUserApi->SetSoftwareVersion("1.1.0"); //设定此软件的开发版本号，用户自定义
	pUserApi->SetSoftwareKey(account_key.c_str());//设定用户的开发代码，在XTP申请开户时，由xtp人员提供
	pUserApi->SetHeartBeatInterval(heat_beat_interval);//设定交易服务器超时时间，单位为秒，此为1.1.16新增接口
	MyTraderSpi* pUserSpi = new MyTraderSpi();
	pUserApi->RegisterSpi(pUserSpi);						// 注册事件类
	pUserSpi->setUserAPI(pUserApi);
	pUserSpi->set_save_to_file(auto_save);
	if (out_count > 0)
	{
		pUserSpi->OutCount(out_count);
	}
	else
	{
		out_count = 1;
	}

	if (account_count > 0)
	{
		///******目前就一个用户测试
		//从配置文件中读取第i个用户登录信息
		account_name = fileUtils->stdStringForKey("account[%d].user", 0);
		account_pw = fileUtils->stdStringForKey("account[%d].password", 0);

		std::cout << account_name << " login begin." << std::endl;
		globalSession_id = pUserApi->Login(trade_server_ip.c_str(), trade_server_port, account_name.c_str(), account_pw.c_str(), XTP_PROTOCOL_TCP); //登录交易服务器

		if (!(globalSession_id > 0))
		{
			//登录失败，获取登录失败原因
			XTPRI* error_info = pUserApi->GetApiLastError();
			std::cout << account_name << " login to server error, " << error_info->error_id << " : " << error_info->error_msg << std::endl;

		}

	}

	if (globalSession_id > 0l)
	{
		//有用户成功登录交易服务器
		std::cout << "Login to server success." << std::endl;

		is_connected_ = true;

		///实例初始化
		Strategy* strategy = nullptr;
		while (!(strategy->Init(settingConfigPath, &myInsertOrder, &myCancelOrder)))
			strategy = nullptr;
		myStrategy = strategy->GetInstance();
		measureInsertOrder = myStrategy->GetTemplate();

		///登录行情服务器成功后，订阅行情
				///***************卖逻辑*************//
		pUserApi->QueryPosition(nullptr, globalSession_id, 1);//订阅账号持股账号
#ifdef _WIN32
		myStrategy->AddPossessTicker("300999", 100);
		myStrategy->SellTrigger("300999", 1000);
		myStrategy->SellTrigger("300999", 10000000);
		myStrategy->SellTrigger("300999", 1000);
#endif

		quote_exchange = fileUtils->intForKey("quote_ticker.exchange");
		pQuoteApi->SubscribeAllMarketData((XTP_EXCHANGE_TYPE)quote_exchange);
		pQuoteApi->SubscribeAllOrderBook((XTP_EXCHANGE_TYPE)quote_exchange);

		///测试
#ifdef _WIN32
		StrategyStart();
#endif

		//主线程循环，防止进程退出
		while (true)
		{
#ifdef _WIN32
			Sleep(5000);//实盘

#else
			sleep(1);
#endif // WIN32

		}
	}


	delete fileUtils;
	fileUtils = NULL;

	if (orderList)
	{
		delete[] orderList;
		orderList = NULL;
	}

	if (session_arrary)
	{
		delete[] session_arrary;
		session_arrary = NULL;
	}

	//等待策略停止
	while (strategy_status)
	{
#ifdef _WIN32
		Sleep(1000);
#else
		sleep(1);
#endif // WIN32
	}

#ifdef _WIN32
	system("pause");
#endif

	return 0;
}
