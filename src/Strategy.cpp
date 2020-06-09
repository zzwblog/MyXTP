#define _CRT_SECURE_NO_WARNINGS
#include "FileUtils.h"
#include "Strategy.h"
#include "Encrypt.h"

using namespace std;

int64_t getCurrentTime(char* out)
{
#ifdef _WIN32
	SYSTEMTIME sys;
	GetLocalTime(&sys);
	sprintf(out, "%02d:%02d:%02d.%03d\0", sys.wHour, sys.wMinute, sys.wSecond, sys.wMilliseconds);
	return (sys.wHour * 3600 * 1000 + sys.wMinute * 60 * 1000 + sys.wSecond * 1000 + sys.wMilliseconds);
#else
	struct timeval    tv;
	struct tm         *p;
	gettimeofday(&tv, nullptr);
	p = localtime(&tv.tv_sec);
	sprintf(out, "%02d:%02d:%02d.%06d\0", p->tm_hour, p->tm_min, p->tm_sec, (int)tv.tv_usec);
	return (p->tm_hour * 3600 * 1000 + p->tm_min * 60 * 1000 + p->tm_sec * 1000 + (int)(tv.tv_usec / 1000));
#endif
}

void getOnlyCurrentTime(char* out)
{
#ifdef _WIN32
	SYSTEMTIME sys;
	GetLocalTime(&sys);
	sprintf(out, "%02d:%02d:%02d.%03d\0", sys.wHour, sys.wMinute, sys.wSecond, sys.wMilliseconds);
#else
	struct timeval    tv;
	struct tm         *p;
	gettimeofday(&tv, nullptr);
	p = localtime(&tv.tv_sec);
	sprintf(out, "%02d:%02d:%02d.%06d\0", p->tm_hour, p->tm_min, p->tm_sec, (int)tv.tv_usec);
#endif
}

void formatTime(int64_t time, char* out)
{
	int year = time / 1e13;
	time %= (int64_t)1e13;
	int month = time / 1e11;
	time %= (int64_t)1e11;
	int day = time / 1e9;
	time %= (int64_t)1e9;
	int hour = time / 1e7;
	time %= (int64_t)1e7;
	int minute = time / 1e5;
	time %= (int64_t)1e5;
	int second = time / 1e3;
	time %= (int64_t)1e3;
	int millisecond = time;
	sprintf(out, "%04d/%02d/%02d %02d:%02d:%02d.%03d\0", year, month, day, hour, minute, second, millisecond);
}
//测速线程
void ResetJustInsertTicker(const std::string& ticker)
{
#ifdef _WIN32
	Sleep(timeWindow);//实盘

#else
	usleep(1000 * timeWindow);
#endif 
	{
		lock_guard<mutex>lock(ourMut);
		our_order_tickers.erase(ticker);
	}
}

//每天数据清除初始化
void DataClear(char* localtime)
{
	{
		lock_guard<mutex>lock(upperMut);
		upper_price_map.clear();//监听涨停价，涨停价要清除，是因为涨停价是新建值，不会覆盖旧值的
	}
	{
		lock_guard<mutex>lock(ourMut);
		our_order_tickers.clear();//我们的下单需要清除昨天的记录
	}
	{
		lock_guard<mutex>lock(otherMut);
		other_order_tickers.clear();//下单成功的触发的对方订单号，需要清除，防止昨天的记录还在
	}
	{
		lock_guard<mutex>lock(tradeMut);
		has_trade_ticker.clear();//表示是否交易，需要清除昨天的记录
	}	
	myStrategy->clear();
	SendMsg(XTP_LOG_LEVEL_WARNING, "Daily DataClear!", localtime);
	cout << "clear.." << endl;
	//现价不用清除，因为现价是赋值会变的，所以第二天会记录新的数据
	//十档申价不用清除，就是因为同样是使用赋值，会覆盖旧记录的
	//possessQtyMut,千万别清除，因为持股订阅只会运行一次，程序先前就把持股信息存下来了，清除了就没有记录了
}

//每天11：00股票抛售
void SellAllTickers(char* localtime)
{
	SendMsg(XTP_LOG_LEVEL_INFO, "It is time to sell all tickers!", localtime);
	cout << "selling.." << endl;
	myStrategy->sellAllTickers();
}

//交易或撤单监听
void doCancelOrTrade(XTPTBT *tbt_data)
{
	auto pDeque = other_order_tickers[tbt_data->ticker];
	for (auto ptr = pDeque.begin(); ptr != pDeque.end(); ptr++)
	{
		if (*ptr == tbt_data->trade.bid_no)//此序列号已经下了单，进行撤单监听
		{
			//传入为交易数据,不是撤单数据,	//还有剩余未交易的委托
			switch (tbt_data->trade.trade_flag)
			{
			case 'F'://成交
				myStrategy->ListenTrade(tbt_data->ticker, tbt_data->trade.bid_no, tbt_data->trade.qty, tbt_data->trade.price);
				break;
			case '4'://撤单
				myStrategy->ListenCancel(tbt_data->ticker, tbt_data->trade.bid_no, tbt_data->trade.qty);
				break;
			default:
				break;
			}
		}
	}
}

//正式下单
uint64_t myInsertOrder(XTPOrderInsertInfo *order, const int64_t &seq) //报单函数, 返回值为0时就是报单失败
{
	int orderAgain = 2; //下单失败就试两次
	uint64_t xtp_id;

	while (orderAgain--)
	{
		xtp_id = pUserApi->InsertOrder(order, globalSession_id);
		//xtp_id = 1;//实盘不要真的下单
		if (xtp_id == 0)
		{
			SendMsg(XTP_LOG_LEVEL_ERROR, "InsertOrder", "Insert fail...");
			XTPRI* error_info = pQuoteApi->GetApiLastError();//获取下单失败类型	
			if (error_info->error_id == 11000381 || error_info->error_id == 11010125)//可用金额不足   余额不足				
				return xtp_id;//金额不足就直接返回			
		}
		else
		{
			//记录时间
			char msg[128];
			char localtime[32];
			getOnlyCurrentTime(localtime);
			sprintf(msg, "\tLocalTime: %s\tOrder tick: %s\tqty: %ld\tprice: %lf\tOrder_uid:%ld",
				localtime, order->ticker, order->quantity, order->price, xtp_id);
			SendMsg(XTP_LOG_LEVEL_INFO, "Insert succeed", msg);
			//保存自己订单
			{
				lock_guard<mutex>lock(ourMut);
				our_order_tickers.insert(make_pair(order->ticker, order->quantity));
			}
			//保留跟单
			{
				lock_guard<mutex>lock(otherMut);
				other_order_tickers[order->ticker].push_back(seq);
			}
			break;
		}
	}
	return xtp_id;
}

//撤单的序列号
void myCancelOrder(const uint64_t order_xtp_id)  //撤单函数
{
	if ((pUserApi->CancelOrder(order_xtp_id, globalSession_id)) == 0)
	{
		printf("Cancel fail!\n");
		SendMsg(XTP_LOG_LEVEL_ERROR, "CancelOrder", "Cancel fail...");
	}
	else
	{
		printf("Cancel succeed!!!!!!!!!!!!!!!!!!\n");
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////
//初始化监听容器
void Strategy::InitListenMap(const std::string& ticker)
{
	{
		lock_guard<mutex>lock(Mut_);
		deque<ListeningTicker>* temp = new deque<ListeningTicker>;
		listeningTickers_.insert(make_pair(ticker.c_str(), temp));
		mutex *m = new mutex;
		mutexs_.insert(make_pair(ticker.c_str(), m));
	}
	{
		lock_guard<mutex>lock(upperLimitBuyMut);
		upperLimitBuy_isArriveThresholds_.insert(make_pair(ticker.c_str(), false));
	}	
}

//监听委托
bool Strategy::ListenEntrust(const int64_t& time, const std::string& ticker, const int64_t& seq, const int64_t &qty, const double& price)
{
	if (has_trade_ticker.find(ticker.c_str()) != has_trade_ticker.end() &&
		has_trade_ticker[ticker.c_str()] == true)
		return false;//已经交易过，一单一天只能跟单一次

	if (blackList_.find(ticker) != blackList_.end())//为黑名单
		return false;
	 
	auto iter = listeningConditions_.find(qty);	
	if (iter == listeningConditions_.end())	//不是要监控的手数
		return false;
	auto condition = iter->second;

	if (book_order_price.find(ticker) != book_order_price.end()//存在监听
		&& !Equ(book_order_price[ticker], 0.00))//当第一档卖价不是为0，则不跟单
		return false;	

	//是否在监控时间内
	auto curTime = GetTime();
	if (curTime < condition.beginTime || curTime > condition.endTime)
		return false;

	//金额是否达到阈值
	if (qty * price - condition.listenOrderValue < 0)
		return false;

	///////////////触发信息打印/////////////////
	char msg[128];
	char localtime[32];
	char ticktime[32];
	formatTime(time, ticktime);
	getOnlyCurrentTime(localtime);
	sprintf(msg, "\ttickTime: %s\tLocalTime: %s\ttigger tick: %s\tseq: %ld\tqty: %ld\tprice: %lf",
		ticktime, localtime, ticker.c_str(), seq, qty, price);
	SendMsg(XTP_LOG_LEVEL_INFO, "Trigger Block", msg);
	//测速监听
	{
		thread th(ResetJustInsertTicker, ticker);
		th.detach();
	}
	/////////////////////////////////////////////
	//监听容器初始化失败
	if (listeningTickers_.find(ticker) == listeningTickers_.end())
	{
		cout << "map init fail!!!" << endl;
		SendMsg(XTP_LOG_LEVEL_FATAL, "InitError", "  Listen Map Init Fail!");
		return false;
	}

	uint64_t xtp_id = InsertOrder(ticker, price, seq, condition.insertOrderValue);
	if (xtp_id == 0)	//如果报单不成功, 则放弃此单
		return false;
	auto* mut = mutexs_[ticker];
	lock_guard<mutex> lock(*mut);
	listeningTickers_[ticker]->emplace_back(seq, qty, condition.dontCancelValue, xtp_id);
	return true;
}

//监听交易
bool Strategy::ListenTrade(const std::string& ticker, const int64_t& seq, const int64_t &qty, const double& price)
{
	auto tempDeq = listeningTickers_[ticker];
	if (tempDeq->empty())//记录为空则返回
		return false;

	auto* mut = mutexs_[ticker];
	lock_guard<mutex> lock(*mut);

	auto curDeq = tempDeq->begin();
	while (curDeq != tempDeq->end() && curDeq->seq != seq)
		curDeq++;
	if (curDeq == tempDeq->end())
		return false;

	if (curDeq->orginPrice != -1.0)
		curDeq->orginPrice = price;
	curDeq->remainQty -= qty;//就是剩余未成交笔数
	curDeq->tradeValue += qty * price;
	//如果全部交易了
	if (curDeq->remainQty == 0)//若果全部成交了，那就不需要撤单
	{
		//记录时间
		char msg[128];
		char localtime[32];
		getOnlyCurrentTime(localtime);
		sprintf(msg, "\tLocalTime: %s\tTrade tick: %s\tseq: %ld\tuid: %ld",
			localtime, ticker.c_str(), seq, curDeq->orderXtpId);
		SendMsg(XTP_LOG_LEVEL_INFO, "all Trade", msg);
		//tempDeq->erase(curDeq);//千万不要删除该条跟踪记录，因为如果你跟2单，跟的2单都先全部交易，你删除记录的话，第二多余的单无法进行撤单，只会被动成交
		return true;
	}
	return false;
}

//监听撤单
bool Strategy::ListenCancel(const std::string& ticker, const int64_t& seq, const int64_t &qty)
{
	auto tempDeq = listeningTickers_[ticker];
	if (tempDeq->empty())//记录为空则返回
		return false;

	auto* mut = mutexs_[ticker];
	lock_guard<mutex> lock(*mut);

	auto curDeq = tempDeq->begin();
	while (curDeq != tempDeq->end() && curDeq->seq != seq)
		curDeq++;
	if (curDeq == tempDeq->end())
		return false;
		
	if (curDeq->remainQty != qty)
	{
		curDeq->tradeValue += (curDeq->remainQty - qty) * curDeq->orginPrice;
	}
	if (curDeq->tradeValue < curDeq->dontCancelThreshold)//如果未超过限制，则可以进行撤单
	{
		cancelOrderFunc_(curDeq->orderXtpId);
	}
	char msg[128];
	char localtime[32];
	getOnlyCurrentTime(localtime);
	sprintf(msg, "\tLocalTime: %s\tCancel tick: %s\tseq: %ld\tuid: %ld",
		localtime, ticker.c_str(), seq, curDeq->orderXtpId);
	SendMsg(XTP_LOG_LEVEL_INFO, "Cancel Order", msg);
	tempDeq->erase(curDeq);//删除已经撤单的记录
	return true;
}

//对已经成功交易了的跟单进行撤单，防止交易多单
void Strategy::CancelTicker(const std::string& ticker, const uint64_t& xtp_id)
{
	for (auto ptr = listeningTickers_[ticker]->begin();
		ptr!= listeningTickers_[ticker]->end();	++ptr)
	{
		if (ptr->orderXtpId == xtp_id)//已成功交易过的订单无需再进行撤单
			continue;
		cancelOrderFunc_(ptr->orderXtpId);//将所有跟单进行撤单，会有一个
		char msg[128];
		char localtime[32];
		getOnlyCurrentTime(localtime);
		sprintf(msg, "\tLocalTime: %s\tCancel tick: %s\tuid: %ld",
			localtime, ticker.c_str(), ptr->orderXtpId);
		SendMsg(XTP_LOG_LEVEL_INFO, "Surplus Cancel Order", msg);
	}
	{
		lock_guard<mutex> lock(*(mutexs_[ticker]));
		listeningTickers_[ticker]->clear();//清除该支股票的跟单记录
	}
	{
		lock_guard<mutex>lock(otherMut);
		other_order_tickers.erase(ticker);//此股票不再跟单，删除跟单记录
	}
	{
		lock_guard<mutex>lock(ourMut);
		our_order_tickers.erase(ticker);//此股票不再跟单，删除下单记录
	}
}

//过夜数据进行清除
void Strategy::clear()
{
	{
		lock_guard<mutex>lock(Mut_);
		listeningTickers_.clear();//之所以全部清除，是因为在重新记录涨停价时会进行重新初始化
		mutexs_.clear();
	}
	{
		lock_guard<mutex>lock(upperLimitBuyMut);
		upperLimitBuy_isArriveThresholds_.clear();
	}	
}

//下单函数
uint64_t Strategy::InsertOrder(const std::string &ticker, const double &price, const int64_t& seq, const int64_t& insertOrderValue)	//返回报单id, 失败则返回0
{
	XTPOrderInsertInfo orderInfo = templateOrder_;
	orderInfo.price = price;
	if (isTest)
		orderInfo.quantity = 100;
	else
	{
		orderInfo.quantity = insertOrderValue / price;
	}
	strcpy(orderInfo.ticker, ticker.c_str());
	return insertOrderFunc_(&orderInfo, seq);
}

bool Strategy::InsertStrategyConfig(long qty, double insertOrderValue, int beginTime, int stopTime, double listenOrderValue, long dontCancelValue)
{
	ListeningCondition listeningCondition;
	listeningCondition.beginTime = beginTime;
	listeningCondition.endTime = stopTime;
	listeningCondition.listenOrderValue = listenOrderValue;
	listeningCondition.dontCancelValue = dontCancelValue;
	listeningCondition.insertOrderValue = insertOrderValue;
	if (!listeningConditions_.insert(make_pair(qty, listeningCondition)).second)
		return false;
	return true;
}

//如果返回空, 则未被初始化
Strategy *Strategy::GetInstance()
{
	return instance_;
}

Strategy *Strategy::instance_ = nullptr;

bool Strategy::Init(std::string path, InsertOrderFunc insertOrder, CancelOrderFunc cancelOrder)
{
	int index = path.find_last_of('.');
	string outputPath = path.substr(0, index) + "Encrypt" + path.substr(index);
	Decrypt(path.c_str(), outputPath.c_str());
	FileUtils file;
	if (!file.init(outputPath.c_str()))
		return false;
	remove(outputPath.c_str());

	Strategy *instance = new Strategy();
	instance_ = instance;
	instance_->cancelOrderFunc_ = cancelOrder;
	instance_->insertOrderFunc_ = insertOrder;
	int listenSize = file.countForKey("strategyConfig");
	instance_->isTest = file.boolForKey("isTesting");
	instance_->tickerUpperLimit_ = file.intForKey("tickerUpperLimit_");
	for (size_t i = 0; i < listenSize; i++)
	{
		long quantity = file.intForKey("strategyConfig[%d].quantity", i) * 100;
		float insertOrderValue = file.intForKey("strategyConfig[%d].insertOrderValue", i) * 10000;
		int startTime = file.intForKey("strategyConfig[%d].startTime", i);
		int stopTime = file.intForKey("strategyConfig[%d].stopTime", i);
		float listenOrderValue = file.intForKey("strategyConfig[%d].listenOrderValue", i) * 10000;
		long dontCancelThreshold = file.intForKey("strategyConfig[%d].dontCancelThreshold", i) * 10000;
		if (!instance->InsertStrategyConfig(quantity, insertOrderValue, startTime, stopTime, listenOrderValue, dontCancelThreshold))
			return false;
	}
	//加入黑名单
	string blackListFile =
#ifdef WIN32
		"..\\..\\api\\ConfigZt1Blacklist.txt"
#else 
		"ConfigZt1Blacklist.txt"
#endif
		;
	index = blackListFile.find_last_of('.');
	outputPath = blackListFile.substr(0, index) + "Encrypt" + blackListFile.substr(index);
	Decrypt(blackListFile.c_str(), outputPath.c_str());
	ifstream fin(outputPath, ios::in);
	if (!fin.is_open())
		return false;
	string line;
	while (getline(fin, line))
	{
		instance_->blackList_.insert(line);
	}
	fin.close();
	remove(outputPath.c_str());
	return true;
}