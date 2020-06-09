#include "quote_spi.h"
#include <iostream>
#include <stdio.h>
#include "xtp_monitor_guest_api.h"

#include "Strategy.h"

using namespace std;

void MyQuoteSpi::OnError(XTPRI *error_info, bool is_last)
{
	cout << "--->>> " << "OnRspError" << endl;
	IsErrorRspInfo(error_info);
}

MyQuoteSpi::MyQuoteSpi()
{
}

MyQuoteSpi::~MyQuoteSpi()
{
}

void MyQuoteSpi::OnDisconnected(int reason)
{
	cout << "--->>> " << "OnDisconnected quote" << endl;
	cout << "--->>> Reason = " << reason << endl;
	//断线后，可以重新连接
	//重新连接成功后，需要重新向服务器发起订阅请求	

	if (strategy_status)//应该不用重新连接服务器
	{
		//重新登录行情服务器
		int loginResult_quote = pQuoteApi->Login(quote_server_ip.c_str(), quote_server_port, quote_username.c_str(), quote_password.c_str(), (XTP_PROTOCOL_TYPE)quote_protocol);
		cout << account_name << " login begin again." << endl;
		//重新登录交易服务器
		globalSession_id = pUserApi->Login(trade_server_ip.c_str(), trade_server_port, account_name.c_str(), account_pw.c_str(), XTP_PROTOCOL_TCP);

		//重新订阅监测
		pQuoteApi->SubscribeAllMarketData((XTP_EXCHANGE_TYPE)quote_exchange);
		pQuoteApi->SubscribeAllOrderBook((XTP_EXCHANGE_TYPE)quote_exchange);
		pQuoteApi->SubscribeAllTickByTick((XTP_EXCHANGE_TYPE)quote_exchange);
	}
	//断线，向监控端发送断线日志
	char strlog[1024] = { 0 };
	sprintf(strlog, "Disconnected from quote.");
	SendMsg(XTP_LOG_LEVEL_ERROR, "Disconnected quote", strlog, 2);
}

void MyQuoteSpi::OnSubMarketData(XTPST *ticker, XTPRI *error_info, bool is_last)
{
	cout << "OnRspSubMarketData -----" << endl;
}

void MyQuoteSpi::OnUnSubMarketData(XTPST *ticker, XTPRI *error_info, bool is_last)
{
	cout << "OnRspUnSubMarketData -----------" << endl;
}

void MyQuoteSpi::OnDepthMarketData(XTPMD * market_data, int64_t bid1_qty[], int32_t bid1_count, int32_t max_bid1_count, int64_t ask1_qty[], int32_t ask1_count, int32_t max_ask1_count)
{
	static bool isInitToday = true;//一开始关闭数据清空状态，防止程序一运行就清空数据
	static bool isAllSellToday = true;//一开始关闭持股抛卖状态，防止程序一运行就卖
	char msg[128];
	char localtime[32];
	int64_t curTime = getCurrentTime(localtime); //更新当时时间
	//每天进行数据清空 //在9.20-9.25之间清空数据  
	///【其实不用对数据进行清理的，因为持订阅函数QueryPosition只会运行一次，所以不可能程序不中断的运行，那样持股信息永远是旧的】
	//if (curTime < (9 * 3600 * 1000 + 23 * 60 * 1000) && curTime >(9 * 3600 * 1000 + 20 * 60 * 1000))
	//{
	//	isInitToday = false;//开启当天允许数据清空状态
	//}
	//if (isInitToday == false && curTime > (9 * 3600 * 1000 + 25 * 60 * 1000))
	//{
	//	isInitToday = true;//关闭当天允许数据清空状态
	//	DataClear(localtime);//只运行一次，不用开线程
	//}

	//每天抛售持股 //在11.20-11.30之间
	if (curTime > (11 * 3600 * 1000 + 15 * 60 * 1000) && curTime < (11 * 3600 * 1000 + 20 * 60 * 1000))
	{
		isAllSellToday = false;//开启当天允许抛售持股
	}
	if (isAllSellToday == false && curTime > (11 * 3600 * 1000 + 20 * 60 * 1000))
	{
		isAllSellToday = true;//关闭当天允许抛售持股
		thread allSell(SellAllTickers, localtime);//因为需要遍历持股列表，需要使用线程
		allSell.detach();
	}

	//监听涨停价
	if (market_data->ticker[0] != '1' &&   //防止股票代码为1开头的债券
		market_data->upper_limit_price > 0.001 && market_data->upper_limit_price < 100000000 &&  //排除一些错误的数据	
		upper_price_map.find(market_data->ticker) == upper_price_map.end())  //不存在就存储，防止重复存入
	{
		//存储涨停价
		{
			lock_guard<mutex>lock(upperMut);
			upper_price_map.insert(make_pair(market_data->ticker, market_data->upper_limit_price));
		}
		//初始化监听容器
		myStrategy->InitListenMap(market_data->ticker);//初始化监听容器
	}

	//监听现价
	{
		lock_guard<mutex>lockt(lastMut);
		last_price_map[market_data->ticker] = market_data->last_price;//之所以不用insert，是因为现价一直在变，要及时更新现价
	}
}

void MyQuoteSpi::OnSubOrderBook(XTPST *ticker, XTPRI *error_info, bool is_last)
{
	cout << "OnSubOrderBook-----------" << endl;
}

void MyQuoteSpi::OnUnSubOrderBook(XTPST *ticker, XTPRI *error_info, bool is_last)
{
	cout << "OnUnSubOrderBook-----------" << endl;
}

void MyQuoteSpi::OnSubTickByTick(XTPST *ticker, XTPRI *error_info, bool is_last)
{
	cout << "OnSubTickByTick-----------" << endl;
}

void MyQuoteSpi::OnUnSubTickByTick(XTPST * ticker, XTPRI * error_info, bool is_last)
{
	cout << "OnUnSubTickByTick-----------" << endl;
}

void MyQuoteSpi::OnOrderBook(XTPOB *order_book)
{
	//请记住，输出的是客户端中设置的value值
	if (order_book->ticker == monitorValue)
	{
		printf("OnOrderBook-----------\ttiker:%s\tlast_price:%lf\tqty:%d\tturnover:%lf\ttrades_count:%ld\ttime%ld\n",
			order_book->ticker, order_book->last_price, order_book->qty, order_book->turnover, order_book->trades_count, order_book->data_time);
		printf("Ten_buy_price:\n");
		for (int i = 0; i < 10; ++i)
			printf("%lf\t", order_book->bid[i]);
		printf("\nTen_buy_qty:\n");
		for (int i = 0; i < 10; ++i)
			printf("%ld\t", order_book->bid_qty[i]);
		printf("\nTen_sell_price:\n");
		for (int i = 0; i < 10; ++i)
			printf("%lf\t", order_book->ask[i]);
		printf("\nTen_sell_qty:\n");
		for (int i = 0; i < 10; ++i)
			printf("%ld\t", order_book->ask_qty[i]);
		printf("\n");
	}
	
	//存储十档申第一档卖价
	{
		lock_guard<mutex>lock(bookMut);
		book_order_price[order_book->ticker] = order_book->ask[0];//不使用insert的原因是，让卖价随时可以更新
	}

	//*********卖逻辑*************//
	if (upper_price_map.find(order_book->ticker)!= upper_price_map.end() &&
		Equ(order_book->bid[0],upper_price_map[order_book->ticker]))//第一档买价等于涨停价
	{
		myStrategy->SellTrigger(order_book->ticker, order_book->bid_qty[0]);
	}
}

void MyQuoteSpi::OnTickByTick(XTPTBT *tbt_data)
{
	//开启测速策略
	if (bIsMeasure &&	//开启测速策略
		tbt_data->type == XTP_TBT_ENTRUST &&	//委托
		tbt_data->entrust.side == '1' &&	//买
		strcmp(tbt_data->ticker, measureInsertOrder.ticker) == 0 //指定股票
		)
	{
		if (tbt_data->entrust.qty >= nMeasureThreshold)	//数量超过阈值
		{
			char msg[128];
			char localtime[32];
			getOnlyCurrentTime(localtime);
			sprintf(msg, "Trigger: LocalTime: %s\tTickerTime: %ld\tOrder tick: %s\tqty: %ld\tprice: %lf",
				localtime, tbt_data->data_time, tbt_data->ticker, tbt_data->entrust.qty, tbt_data->entrust.price);
			SendMsg(XTP_LOG_LEVEL_TRACE, "Measure", msg);
			pUserApi->InsertOrder(&measureInsertOrder, globalSession_id);//下单
		}
		else if (tbt_data->entrust.qty == measureInsertOrder.quantity &&
			Equ(tbt_data->entrust.price, measureInsertOrder.price))
		{
			char msg[128];
			char localtime[32];
			getOnlyCurrentTime(localtime);
			sprintf(msg, "Follow: LocalTime: %s\tTickerTime: %ld\tOrder tick: %s\tqty: %ld\tprice: %lf",
				localtime, tbt_data->data_time, tbt_data->ticker, tbt_data->entrust.qty, tbt_data->entrust.price);
			SendMsg(XTP_LOG_LEVEL_TRACE, "Measure", msg);
		}
	}

	//跟单测速
	//使用线程卡表
	if (tbt_data->type == XTP_TBT_ENTRUST && //是委托		
		our_order_tickers.find(tbt_data->ticker) != our_order_tickers.end() &&//相同股票代码
		Equ(tbt_data->entrust.price, upper_price_map[tbt_data->ticker]) &&//相同价格
		tbt_data->entrust.qty == our_order_tickers[tbt_data->ticker])//相同数量
	{
		char msg[128];
		char localtime[32];
		getOnlyCurrentTime(localtime);
		sprintf(msg, "\tLocalTime: %s\tOrder tick: %s\tqty: %ld\tprice: %lf",
			localtime, tbt_data->ticker, tbt_data->entrust.qty, tbt_data->entrust.price);
		SendMsg(XTP_LOG_LEVEL_INFO, "timeListening", msg);
	}
	//使用简单的时间
	//char msg[128];	
	//char localtime[32];
	//int64_t curTime = getCurrentTime(localtime);
	//if ((curTime - updateTime) >= 0 && (curTime - updateTime) <= timeWindow &&  //在时间窗口内
	//	tbt_data->type == XTP_TBT_ENTRUST && //是委托		
	//	our_order_tickers.find(tbt_data->ticker) != our_order_tickers.end() &&//相同股票代码
	//	Equ(tbt_data->entrust.price, upper_price_map[tbt_data->ticker]) &&//相同价格
	//	tbt_data->entrust.qty == our_order_tickers[tbt_data->ticker])//相同数量
	//{
	//	sprintf(msg, "\tLocalTime: %s\tOrder tick: %s\tqty: %ld\tprice: %lf",
	//		localtime, tbt_data->ticker, tbt_data->entrust.qty, tbt_data->entrust.price);
	//	SendMsg(XTP_LOG_LEVEL_INFO, "timeListening", msg);
	//}

	//监听委托进行跟单
	if (tbt_data->type == XTP_TBT_ENTRUST && tbt_data->entrust.side == '1' &&  //是属于委托   且委托属于买入  
		upper_price_map.find(tbt_data->ticker) != upper_price_map.end() &&
		Equ(upper_price_map[tbt_data->ticker], tbt_data->entrust.price)) //委托等于涨停价
	{
		//跟单
		myStrategy->ListenEntrust(tbt_data->data_time, tbt_data->ticker, tbt_data->entrust.seq, tbt_data->entrust.qty, tbt_data->entrust.price);
	}

	//监听交易进行撤单
	if (tbt_data->type == XTP_TBT_TRADE && //是交易
		other_order_tickers.find(tbt_data->ticker) != other_order_tickers.end()) //存了这一单
	{
		//此处使用线程，应为监听的他人单是使用queue存储，需要一个查找，故使用线程好一点
		thread tradeTh(doCancelOrTrade, tbt_data);
		tradeTh.detach();
	}	
}

void MyQuoteSpi::OnQueryAllTickers(XTPQSI * ticker_info, XTPRI * error_info, bool is_last)
{
	cout << "OnQueryAllTickers -----------" << endl;
}

void MyQuoteSpi::OnQueryTickersPriceInfo(XTPTPI * ticker_info, XTPRI * error_info, bool is_last)
{
	cout << "OnQueryTickersPriceInfo-----------" << endl;
}

void MyQuoteSpi::OnSubscribeAllMarketData(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnSubscribeAllMarketData -----------" << endl;
}

void MyQuoteSpi::OnUnSubscribeAllMarketData(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnUnSubscribeAllMarketData -----------" << endl;
}

void MyQuoteSpi::OnSubscribeAllOrderBook(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnSubscribeAllOrderBook -----------" << endl;
}

void MyQuoteSpi::OnUnSubscribeAllOrderBook(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnUnSubscribeAllOrderBook -----------" << endl;
}

void MyQuoteSpi::OnSubscribeAllTickByTick(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnSubscribeAllTickByTick -----------" << endl;
}

void MyQuoteSpi::OnUnSubscribeAllTickByTick(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnUnSubscribeAllTickByTick -----------" << endl;
}

void MyQuoteSpi::OnSubscribeAllOptionMarketData(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnSubscribeAllOptionMarketData -----------" << endl;
}

void MyQuoteSpi::OnUnSubscribeAllOptionMarketData(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnUnSubscribeAllOptionMarketData -----------" << endl;
}

void MyQuoteSpi::OnSubscribeAllOptionOrderBook(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnSubscribeAllOptionOrderBook -----------" << endl;
}

void MyQuoteSpi::OnUnSubscribeAllOptionOrderBook(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnUnSubscribeAllOptionOrderBook -----------" << endl;
}

void MyQuoteSpi::OnSubscribeAllOptionTickByTick(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnSubscribeAllOptionTickByTick -----------" << endl;
}

void MyQuoteSpi::OnUnSubscribeAllOptionTickByTick(XTP_EXCHANGE_TYPE exchange_id, XTPRI * error_info)
{
	cout << "OnUnSubscribeAllOptionTickByTick -----------" << endl;
}

bool MyQuoteSpi::IsErrorRspInfo(XTPRI *pRspInfo)
{
	// 如果ErrorID != 0, 说明收到了错误的响应
	bool bResult = ((pRspInfo) && (pRspInfo->error_id != 0));
	if (bResult)
		cout << "--->>> ErrorID=" << pRspInfo->error_id << ", ErrorMsg=" << pRspInfo->error_msg << endl;
	return bResult;
}

