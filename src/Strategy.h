//#pragma once
#include "xoms_api_struct.h"
#include "xtp_api_struct.h"
#include "xtp_quote_api.h"
#include "xtp_trader_api.h"
#include "xtp_monitor_guest_api.h"
#include "FileUtils.h"

#include <iostream>
#include <fstream>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <deque>
#include <vector>
#include <mutex>
#include <math.h>
#include <thread>
#define MapInitSize 5000
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

class FileUtils;
extern FileUtils* fileUtils;
class Strategy;
extern Strategy* myStrategy;

extern XTP::API::QuoteApi* pQuoteApi;
extern XTP::API::TraderApi* pUserApi;
extern std::string monitorValue;
extern int quote_exchange;
extern double Eps;
extern int64_t updateTime, timeWindow;
extern int64_t SellThresholds;//卖策略阈值
extern bool strategy_status;
extern uint64_t globalSession_id;
extern std::mutex upperMut, lastMut, bookMut, otherMut, ourMut, tradeMut; //互斥锁
extern std::unordered_map<std::string, double> upper_price_map;//监听涨停价
extern std::unordered_map<std::string, double> last_price_map;//监听最新价，以最新价卖出
extern std::unordered_map<std::string, double> book_order_price;//十档申第一档卖价为0时才允许跟单
extern std::unordered_map<std::string, std::deque<int64_t>> other_order_tickers;//下单成功的触发的对方订单号
extern std::unordered_map<std::string, bool>has_trade_ticker;	//表示是否交易
extern std::unordered_map<std::string, int64_t>our_order_tickers;//用来存储我们的委托订单
//*************************测速**********
extern bool bIsMeasure;
extern long long nMeasureThreshold;
extern XTPOrderInsertInfo measureInsertOrder;
///************************************

#define Equ(a,b) ((fabs((a)-(b))) < (Eps))  //在精确度范围内即可
uint64_t myInsertOrder(XTPOrderInsertInfo *order, const int64_t &seq);	 //报单函数, 返回值为0时就是报单失败
//double myGetPrice(const std::string& ticker);   //获取价格函数
void myCancelOrder(const uint64_t order_xtp_id);  //撤单函数
int64_t getCurrentTime(char* out);
void getOnlyCurrentTime(char* out);
void formatTime(int64_t time, char* out);
void doCancelOrTrade(XTPTBT *tbt_data);//监听撤单
void DataClear(char* localtime);
void SellAllTickers(char* localtime);
void ResetJustInsertTicker(const std::string& ticker);//测速线程
//enum Opera {
//	insert, trade, cancel
//};

//有几种情况会控制不了

class Strategy final
{
	//静态函数是非线程安全的
	using InsertOrderFunc = uint64_t(*) (XTPOrderInsertInfo *, const int64_t& seq); //报单函数, 返回值为0时就是报单失败
	using GetPriceFunc = double(*) (const std::string& ticker);   //获取价格函数
	using CancelOrderFunc = void(*) (const uint64_t order_xtp_id);  //撤单函数
public:
	static Strategy* GetInstance();
	//path 是 setting.json
	//后面三个是函数
	static bool Init(std::string path, InsertOrderFunc insertOrder, CancelOrderFunc cancelOrder);	//如果不成功就用Clear重置再弄

	//根据listenEntrust返回的正负, 得到需要监听的交易的序列号
	bool ListenEntrust(const int64_t& time, const std::string& ticker, const int64_t& seq, const int64_t &qty, const double& price);	//委托
	//若返回true 则表示该笔委托全部交易完成
	bool ListenTrade(const std::string& ticker, const int64_t& seq, const int64_t &qty, const double& price);	//交易

	bool ListenCancel(const std::string& ticker, const int64_t& seq, const int64_t &qty);	//撤单

	void InitListenMap(const std::string& ticker);//初始化监听容器

	void CancelTicker(const std::string& ticker, const uint64_t& xtp_id);

	void clear();//清空过夜的数据

	const XTPOrderInsertInfo& GetTemplate() const { return templateOrder_; }

private:
	bool InsertStrategyConfig(long qty, double insertOrderValue, int beginTime, int stopTime, double listenOrderValue, long dontCancelValue);

	Strategy() : Mut_(), listeningTickers_(MapInitSize), mutexs_(MapInitSize)
	{
		templateOrder_.order_client_id = fileUtils->intForKey("client_id");
		templateOrder_.market = (XTP_MARKET_TYPE)fileUtils->intForKey("order[0].exchange");
		templateOrder_.price_type = (XTP_PRICE_TYPE)fileUtils->intForKey("order[0].price_type");
		templateOrder_.side = (XTP_SIDE_TYPE)fileUtils->intForKey("order[0].side");
		templateOrder_.position_effect = (XTP_POSITION_EFFECT_TYPE)fileUtils->intForKey("order[0].position_effect");
		templateOrder_.business_type = (XTP_BUSINESS_TYPE)fileUtils->intForKey("order[0].business_type");
	}

	struct ListeningCondition
	{
		float insertOrderValue;//下单的金额
		int beginTime;//监听开始时间
		int endTime;//监听结束时间
		double listenOrderValue;//最低金额
		double dontCancelValue;//当监听委托成交多少后, 即便委托撤单, 我们也不跟随撤单
	};

	struct ListeningTicker
	{
		int64_t seq;		//唯一编号, 用于识别
		int64_t remainQty;	//剩余的交易数量
		double tradeValue;	//已经交易了多少金额
		double orginPrice;
		int64_t dontCancelThreshold;
		uint64_t orderXtpId;
		static Strategy *instance_;
		ListeningTicker(int64_t s, int64_t r, int64_t d, uint64_t o) : seq(s), remainQty(r), tradeValue(0.0), orginPrice(-1.0), dontCancelThreshold(d), orderXtpId(o) {}
	};

	////////////////////////////////////////////////////////////////////////////////////////////
	//初始化函数直接初始的数据成员
	static Strategy *instance_;
	InsertOrderFunc insertOrderFunc_;
	CancelOrderFunc cancelOrderFunc_;

	//从配置文件中读取的数据成员(以手数被id)
	bool isTest;	//如果是测试, 则下单数量总是100股
	int tickerUpperLimit_;	//表示有最多可以监听多少个委托(同一只股
	XTPOrderInsertInfo templateOrder_;

	//黑名单
	std::unordered_set<std::string> blackList_;
	//监听要求
	std::unordered_map<long, ListeningCondition> listeningConditions_;
	//监听队列
	std::unordered_map<std::string, std::deque<ListeningTicker>*> listeningTickers_; //正在被监控得委托
	std::mutex Mut_;	//互斥访问listeningTickers_
	std::unordered_map<std::string, std::mutex*>  mutexs_;	//互斥访问listeningTickers_中的各个vector

	//清除指定股票
	void Erase(std::vector<ListeningTicker>::iterator &ticker, std::vector<ListeningTicker>* target)	//线程不安全
	{
		ticker = target->erase(ticker);
	}

	//报单调用函数
	uint64_t InsertOrder(const std::string &ticker, const double &price, const int64_t& seq, const int64_t& insertOrderValue);	//返回报单id, 失败则返回0

	int GetTime()
	{
#ifdef _WIN32
		SYSTEMTIME sys;
		GetLocalTime(&sys);
		return sys.wHour * 100 + sys.wMinute;
#else
		time_t tt = time(NULL);
		struct tm *p = localtime(&tt);
		return p->tm_hour * 100 + p->tm_min;
#endif
	}
	//******************卖逻辑******************//
private:
	std::mutex upperLimitBuyMut, possessQtyMut;
	std::unordered_map<std::string, bool> upperLimitBuy_isArriveThresholds_;
	std::unordered_map<std::string, int> possessQty_;	//手上拥有的数量,若不持有改股则为0

public:
	void AddPossessTicker(const std::string &ticker, const int64_t &qty)
	{
		std::lock_guard<std::mutex>lock(possessQtyMut);
		possessQty_[ticker] = qty;
	}

	void SellTrigger(const std::string &ticker, const int64_t &qty)
	{
		if (possessQty_.find(ticker) == possessQty_.end() || possessQty_[ticker] == 0)
			return;
		if (upperLimitBuy_isArriveThresholds_.find(ticker)!= upperLimitBuy_isArriveThresholds_.end() &&
			upperLimitBuy_isArriveThresholds_[ticker] == true && qty < SellThresholds)
		{
			//TODO 卖掉
			XTPOrderInsertInfo sellOrder = templateOrder_;
			sellOrder.side = XTP_SIDE_SELL;
			sellOrder.price = upper_price_map[ticker];
			sellOrder.quantity = possessQty_[ticker];
			strcpy(sellOrder.ticker, ticker.c_str());
			for (size_t i = 0; i < 2; i++)		//失败则重新下单，只尝试两次
			{
				if (pUserApi->InsertOrder(&sellOrder, globalSession_id) != 0)
				{
					char msg[128];
					char localtime[32];
					getOnlyCurrentTime(localtime);
					sprintf(msg, "\tLocalTime: %s\tOrder tick: %s\tqty: %ld", localtime, ticker.c_str(), qty);
					SendMsg(XTP_LOG_LEVEL_INFO, "Sell Strategy", msg);
					{
						std::lock_guard<std::mutex>lock(possessQtyMut);
						possessQty_[ticker] = 0;
					}
					{
						std::lock_guard<std::mutex>lock(upperLimitBuyMut);
						upperLimitBuy_isArriveThresholds_[ticker] = false;
					}
					break;
				}
			}
		}
		else if (qty >= SellThresholds)
		{
			std::lock_guard<std::mutex>lock(upperLimitBuyMut);
			upperLimitBuy_isArriveThresholds_[ticker] = true;
		}
	}

	//按时卖单，每天11.20将所有能卖持股卖出：
	void sellAllTickers()
	{
		for (auto ptr = possessQty_.begin(); ptr != possessQty_.end(); ++ptr)
		{
			if (ptr->second == 0)continue;
			printf("ticker: %s\t qty:%d\n", ptr->first.c_str(), ptr->second);
			
			XTPOrderInsertInfo sellOrder = templateOrder_;
			sellOrder.side = XTP_SIDE_SELL;
			sellOrder.price = last_price_map[ptr->first];//以现价卖出
			sellOrder.quantity = ptr->second;//可卖 出股全部卖出
			strcpy(sellOrder.ticker, ptr->first.c_str());
			for (size_t i = 0; i < 2; i++)		//失败则重新下单，只尝试两次
			{
				if (pUserApi->InsertOrder(&sellOrder, globalSession_id) != 0)
				{
					char msg[128];
					char localtime[32];
					getOnlyCurrentTime(localtime);
					sprintf(msg, "\tLocalTime: %s\tOrder tick: %s\tqty: %ld", localtime, ptr->first.c_str(), ptr->second);
					SendMsg(XTP_LOG_LEVEL_INFO, "my All_Sell Succeed!", msg);
					{
						std::lock_guard<std::mutex>lock(possessQtyMut);
						possessQty_[ptr->first] = 0;
					}
					{
						std::lock_guard<std::mutex>lock(upperLimitBuyMut);
						upperLimitBuy_isArriveThresholds_[ptr->first] = false;
					}
					break;
				}
				else if (i == 1)//第二次下单失败
				{
					char msg[128];
					char localtime[32];
					getOnlyCurrentTime(localtime);
					sprintf(msg, "\tLocalTime: %s\tOrder tick: %s\tqty: %ld", localtime, ptr->first.c_str(), ptr->second);
					SendMsg(XTP_LOG_LEVEL_ERROR, "my All_Sell Failed!", msg);
				}
			}			
		}
	}
};


