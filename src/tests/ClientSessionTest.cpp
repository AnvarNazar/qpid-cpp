/*
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */
#include "unit_test.h"
#include "BrokerFixture.h"
#include "qpid/client/SubscriptionManager.h"
#include "qpid/sys/Monitor.h"
#include "qpid/sys/Thread.h"
#include "qpid/sys/Runnable.h"
#include "qpid/client/Session.h"
#include "qpid/framing/TransferContent.h"
#include "qpid/framing/reply_exceptions.h"

#include <boost/optional.hpp>
#include <boost/lexical_cast.hpp>

#include <vector>

QPID_AUTO_TEST_SUITE(ClientSessionTest)

using namespace qpid::client;
using namespace qpid::framing;
using namespace qpid;
using qpid::sys::Monitor;
using qpid::sys::Thread;
using qpid::sys::TIME_SEC;
using qpid::broker::Broker;
using std::string;
using std::cout;
using std::endl;


struct DummyListener : public sys::Runnable, public MessageListener {
    std::vector<Message> messages;
    string name;
    uint expected;
    SubscriptionManager submgr;

    DummyListener(Session& session, const string& n, uint ex) :
        name(n), expected(ex), submgr(session) {}

    void run()
    {
        submgr.subscribe(*this, name);
        submgr.run();
    }

    void received(Message& msg)
    {
        messages.push_back(msg);
        if (--expected == 0) {
            submgr.stop();
        }
    }
};

struct SimpleListener : public MessageListener
{
    Monitor lock;
    std::vector<Message> messages;

    void received(Message& msg)
    {
        Monitor::ScopedLock l(lock);
        messages.push_back(msg);
        lock.notifyAll();
    }

    void waitFor(const uint n)
    {
        Monitor::ScopedLock l(lock);
        while (messages.size() < n) {
            lock.wait();
        }
    }
};

struct ClientSessionFixture : public ProxySessionFixture
{
    ClientSessionFixture(Broker::Options opts = Broker::Options()) : ProxySessionFixture(opts) {
        session.queueDeclare(arg::queue="my-queue");
    }
};

QPID_AUTO_TEST_CASE(testQueueQuery) {
    ClientSessionFixture fix;
    fix.session = fix.connection.newSession();
    fix.session.queueDeclare(arg::queue="q", arg::alternateExchange="amq.fanout",
                             arg::exclusive=true, arg::autoDelete=true);
    QueueQueryResult result = fix.session.queueQuery("q");
    BOOST_CHECK_EQUAL(false, result.getDurable());
    BOOST_CHECK_EQUAL(true, result.getExclusive());
    BOOST_CHECK_EQUAL("amq.fanout", result.getAlternateExchange());
}

QPID_AUTO_TEST_CASE(testDispatcher)
{
    ClientSessionFixture fix;
    fix.session =fix.connection.newSession();
    size_t count = 100;
    for (size_t i = 0; i < count; ++i) 
        fix.session.messageTransfer(arg::content=TransferContent(boost::lexical_cast<string>(i), "my-queue"));
    DummyListener listener(fix.session, "my-queue", count);
    listener.run();
    BOOST_CHECK_EQUAL(count, listener.messages.size());        
    for (size_t i = 0; i < count; ++i) 
        BOOST_CHECK_EQUAL(boost::lexical_cast<string>(i), listener.messages[i].getData());
}

QPID_AUTO_TEST_CASE(testDispatcherThread)
{
    ClientSessionFixture fix;
    fix.session =fix.connection.newSession();
    size_t count = 10;
    DummyListener listener(fix.session, "my-queue", count);
    sys::Thread t(listener);
    for (size_t i = 0; i < count; ++i) {
        fix.session.messageTransfer(arg::content=TransferContent(boost::lexical_cast<string>(i), "my-queue"));
    }
    t.join();
    BOOST_CHECK_EQUAL(count, listener.messages.size());        
    for (size_t i = 0; i < count; ++i) 
        BOOST_CHECK_EQUAL(boost::lexical_cast<string>(i), listener.messages[i].getData());
}

QPID_AUTO_TEST_CASE_EXPECTED_FAILURES(testSuspend0Timeout, 1)
{
    ClientSessionFixture fix;
    fix.session.suspend();  // session has 0 timeout.
    try {
        fix.connection.resume(fix.session);
        BOOST_FAIL("Expected InvalidArgumentException.");
    } catch(const InternalErrorException&) {}
}

QPID_AUTO_TEST_CASE(testUseSuspendedError)
{
    ClientSessionFixture fix;
    fix.session.timeout(60);
    fix.session.suspend();
    try {
        fix.session.exchangeQuery(arg::exchange="amq.fanout");
        BOOST_FAIL("Expected session suspended exception");
    } catch(const NotAttachedException&) {}
}

QPID_AUTO_TEST_CASE_EXPECTED_FAILURES(testSuspendResume, 1)
{
    ClientSessionFixture fix;
    fix.session.timeout(60);
    fix.session.suspend();
    // Make sure we are still subscribed after resume.
    fix.connection.resume(fix.session);
    fix.session.messageTransfer(arg::content=TransferContent("my-message", "my-queue"));
    FrameSet::shared_ptr msg = fix.session.get();
    BOOST_CHECK_EQUAL(string("my-message"), msg->getContent());
}


QPID_AUTO_TEST_CASE(testSendToSelf) {
    ClientSessionFixture fix;
    SimpleListener mylistener;
    fix.session.queueDeclare(arg::queue="myq", arg::exclusive=true, arg::autoDelete=true);
    fix.subs.subscribe(mylistener, "myq");
    sys::Thread runner(fix.subs);//start dispatcher thread
    string data("msg");
    Message msg(data, "myq");
    const uint count=10;
    for (uint i = 0; i < count; ++i) {
        fix.session.messageTransfer(arg::content=msg);
    }
    mylistener.waitFor(count);
    fix.subs.cancel("myq");
    fix.subs.stop();
    runner.join();
    fix.session.close();
    BOOST_CHECK_EQUAL(mylistener.messages.size(), count);
    for (uint j = 0; j < count; ++j) {
        BOOST_CHECK_EQUAL(mylistener.messages[j].getData(), data);
    }
}

QPID_AUTO_TEST_CASE(testLocalQueue) {
    ClientSessionFixture fix;
    fix.session.queueDeclare(arg::queue="lq", arg::exclusive=true, arg::autoDelete=true);
    LocalQueue lq;
    fix.subs.subscribe(lq, "lq", FlowControl(2, FlowControl::UNLIMITED, false));
    fix.session.messageTransfer(arg::content=Message("foo0", "lq"));
    fix.session.messageTransfer(arg::content=Message("foo1", "lq"));
    fix.session.messageTransfer(arg::content=Message("foo2", "lq"));
    BOOST_CHECK_EQUAL("foo0", lq.pop().getData());
    BOOST_CHECK_EQUAL("foo1", lq.pop().getData());
    BOOST_CHECK(lq.empty());    // Credit exhausted.
    fix.subs.getSubscription("lq").setFlowControl(FlowControl::unlimited());
    BOOST_CHECK_EQUAL("foo2", lq.pop().getData());    
}

struct DelayedTransfer : sys::Runnable
{
    ClientSessionFixture& fixture;

    DelayedTransfer(ClientSessionFixture& f) : fixture(f) {}

    void run()
    {
        sleep(1);
        fixture.session.messageTransfer(arg::content=Message("foo2", "getq"));
    }
};

QPID_AUTO_TEST_CASE(testGet) {
    ClientSessionFixture fix;
    fix.session.queueDeclare(arg::queue="getq", arg::exclusive=true, arg::autoDelete=true);
    fix.session.messageTransfer(arg::content=Message("foo0", "getq"));
    fix.session.messageTransfer(arg::content=Message("foo1", "getq"));
    Message got;
    BOOST_CHECK(fix.subs.get(got, "getq", TIME_SEC));
    BOOST_CHECK_EQUAL("foo0", got.getData());
    BOOST_CHECK(fix.subs.get(got, "getq", TIME_SEC));
    BOOST_CHECK_EQUAL("foo1", got.getData());
    BOOST_CHECK(!fix.subs.get(got, "getq"));
    DelayedTransfer sender(fix);
    Thread t(sender);
    //test timed get where message shows up after a short delay
    BOOST_CHECK(fix.subs.get(got, "getq", 5*TIME_SEC));
    BOOST_CHECK_EQUAL("foo2", got.getData());    
    t.join();
}

QPID_AUTO_TEST_CASE(testOpenFailure) {
    BrokerFixture b;
    Connection c;
    string host("unknowable-host");
    try {
        c.open(host);
    } catch (const Exception&) {
        BOOST_CHECK(!c.isOpen());
    }
    b.open(c);
    BOOST_CHECK(c.isOpen());
    c.close();
    BOOST_CHECK(!c.isOpen());
}

QPID_AUTO_TEST_CASE(testPeriodicExpiration) {
    Broker::Options opts;
    opts.queueCleanInterval = 1;
    ClientSessionFixture fix(opts);
    fix.session.queueDeclare(arg::queue="my-queue", arg::exclusive=true, arg::autoDelete=true);

    for (uint i = 0; i < 10; i++) {        
        Message m((boost::format("Message_%1%") % (i+1)).str(), "my-queue");        
        if (i % 2) m.getDeliveryProperties().setTtl(500);
        fix.session.messageTransfer(arg::content=m);
    }

    BOOST_CHECK_EQUAL(fix.session.queueQuery(string("my-queue")).getMessageCount(), 10u);
    sleep(2);
    BOOST_CHECK_EQUAL(fix.session.queueQuery(string("my-queue")).getMessageCount(), 5u);
}

QPID_AUTO_TEST_CASE(testExpirationOnPop) {
    ClientSessionFixture fix;
    fix.session.queueDeclare(arg::queue="my-queue", arg::exclusive=true, arg::autoDelete=true);

    for (uint i = 0; i < 10; i++) {        
        Message m((boost::format("Message_%1%") % (i+1)).str(), "my-queue");        
        if (i % 2) m.getDeliveryProperties().setTtl(200);
        fix.session.messageTransfer(arg::content=m);
    }

    ::usleep(300* 1000);

    for (uint i = 0; i < 10; i++) {        
        if (i % 2) continue;
        Message m;
        BOOST_CHECK(fix.subs.get(m, "my-queue", TIME_SEC));
        BOOST_CHECK_EQUAL((boost::format("Message_%1%") % (i+1)).str(), m.getData());
    }
}

QPID_AUTO_TEST_CASE(testRelease) {
    ClientSessionFixture fix;

    const uint count=10;
    for (uint i = 0; i < count; i++) {        
        Message m((boost::format("Message_%1%") % (i+1)).str(), "my-queue");        
        fix.session.messageTransfer(arg::content=m);
    }

    fix.subs.setAutoStop(false);
    sys::Thread runner(fix.subs);//start dispatcher thread
    SubscriptionSettings settings;
    settings.autoAck = 0;

    SimpleListener l1;
    Subscription s1 = fix.subs.subscribe(l1, "my-queue", settings);
    l1.waitFor(count);
    s1.cancel();

    for (uint i = 0; i < count; i++) {
        BOOST_CHECK_EQUAL((boost::format("Message_%1%") % (i+1)).str(), l1.messages[i].getData());
    }
    s1.release(s1.getUnaccepted());

    //check that released messages are redelivered
    settings.autoAck = 1;
    SimpleListener l2;
    Subscription s2 = fix.subs.subscribe(l2, "my-queue", settings);
    l2.waitFor(count);
    for (uint i = 0; i < count; i++) {
        BOOST_CHECK_EQUAL((boost::format("Message_%1%") % (i+1)).str(), l2.messages[i].getData());
    }
    
    fix.subs.stop();
    runner.join();
    fix.session.close();
}

QPID_AUTO_TEST_CASE(testCompleteOnAccept) {
    ClientSessionFixture fix;

    fix.session.queueDeclare(arg::queue="HELP_FIND_ME");

    const uint count = 8;
    const uint chunk = 4;
    for (uint i = 0; i < count; i++) {        
        Message m((boost::format("Message_%1%") % (i+1)).str(), "my-queue");        
        fix.session.messageTransfer(arg::content=m);
    }

    SubscriptionSettings settings;
    settings.autoAck = 0;
    settings.completionMode = COMPLETE_ON_ACCEPT;
    settings.flowControl = FlowControl::messageWindow(chunk);

    LocalQueue q;
    Subscription s = fix.subs.subscribe(q, "my-queue", settings);
    fix.session.messageFlush(arg::destination=s.getName());
    SequenceSet accepted;
    for (uint i = 0; i < chunk; i++) {        
        Message m;
        BOOST_CHECK(q.get(m));
        BOOST_CHECK_EQUAL((boost::format("Message_%1%") % (i+1)).str(), m.getData());
        accepted.add(m.getId());
    }    
    Message m;
    BOOST_CHECK(!q.get(m));
    
    s.accept(accepted);
    fix.session.messageFlush(arg::destination=s.getName());
    accepted.clear();
    
    for (uint i = chunk; i < count; i++) {        
        Message m;
        BOOST_CHECK(q.get(m));
        BOOST_CHECK_EQUAL((boost::format("Message_%1%") % (i+1)).str(), m.getData());
        accepted.add(m.getId());
    }    
    fix.session.messageAccept(accepted);

    fix.session.queueDelete(arg::queue="HELP_FIND_ME");

}

QPID_AUTO_TEST_SUITE_END()


