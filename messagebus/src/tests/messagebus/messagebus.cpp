// Copyright 2016 Yahoo Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/messagebus/destinationsession.h>
#include <vespa/messagebus/emptyreply.h>
#include <vespa/messagebus/error.h>
#include <vespa/messagebus/errorcode.h>
#include <vespa/messagebus/intermediatesession.h>
#include <vespa/messagebus/messagebus.h>
#include <vespa/messagebus/routablequeue.h>
#include <vespa/messagebus/routing/route.h>
#include <vespa/messagebus/routing/routingcontext.h>
#include <vespa/messagebus/routing/routingnodeiterator.h>
#include <vespa/messagebus/routing/routingspec.h>
#include <vespa/messagebus/sourcesession.h>
#include <vespa/messagebus/sourcesessionparams.h>
#include <vespa/messagebus/testlib/receptor.h>
#include <vespa/messagebus/testlib/simplemessage.h>
#include <vespa/messagebus/testlib/simpleprotocol.h>
#include <vespa/messagebus/testlib/simplereply.h>
#include <vespa/messagebus/testlib/slobrok.h>
#include <vespa/messagebus/testlib/testserver.h>
#include <vespa/vespalib/testkit/testapp.h>
#include <vespa/vespalib/util/vstringfmt.h>

using namespace mbus;

struct Base {
    RoutableQueue queue;
    Base() : queue() {}
    virtual ~Base() {
        while (queue.size() > 0) {
            Routable::UP r = queue.dequeue(0);
            r->getCallStack().discard();
        }
    }
    RoutingSpec getRouting() {
        return RoutingSpec()
            .addTable(RoutingTableSpec("Simple")
                      .addHop(HopSpec("DocProc", "docproc/*/session"))
                      .addHop(HopSpec("Search", "search/[All]/[Hash]/session")
                              .addRecipient("search/r.0/c.0/session")
                              .addRecipient("search/r.0/c.1/session")
                              .addRecipient("search/r.1/c.0/session")
                              .addRecipient("search/r.1/c.1/session"))
                      .addRoute(RouteSpec("Index").addHop("DocProc").addHop("Search"))
                      .addRoute(RouteSpec("DocProc").addHop("DocProc"))
                      .addRoute(RouteSpec("Search").addHop("Search")));
    }
    bool waitQueueSize(uint32_t size) {
        for (uint32_t i = 0; i < 1000; ++i) {
            if (queue.size() == size) {
                return true;
            }
            FastOS_Thread::Sleep(10);
        }
        return false;
    }
};

struct Client : public Base {
    typedef std::unique_ptr<Client> UP;
    TestServer        server;
    SourceSession::UP session;
    Client(Slobrok &slobrok)
        : Base(), server(Identity(""), getRouting(), slobrok), session()
    {
        SourceSessionParams params;
        params.setThrottlePolicy(IThrottlePolicy::SP());
        session = server.mb.createSourceSession(queue, params);

    }
};

struct Server : public Base {
    TestServer    server;
    Server(const string &name, Slobrok &slobrok)
        : Base(), server(Identity(name), getRouting(), slobrok)
    {
        // empty
    }
};

struct DocProc : public Server {
    typedef std::unique_ptr<DocProc> UP;
    IntermediateSession::UP session;
    DocProc(const string &name, Slobrok &slobrok)
        : Server(name, slobrok), session()
    {
        session = server.mb.createIntermediateSession("session", true, queue, queue);
    }
};

struct Search : public Server {
    typedef std::unique_ptr<Search> UP;
    DestinationSession::UP session;
    Search(const string &name, Slobrok &slobrok)
        : Server(name, slobrok), session()
    {
        session = server.mb.createDestinationSession("session", true, queue);
    }
};

//-----------------------------------------------------------------------------

class Test : public vespalib::TestApp {
private:
    Slobrok::UP  slobrok;
    Client::UP   client;
    DocProc::UP  dp0;
    DocProc::UP  dp1;
    DocProc::UP  dp2;
    Search::UP   search00;
    Search::UP   search01;
    Search::UP   search10;
    Search::UP   search11;
    std::vector<DocProc*> dpVec;
    std::vector<Search*>  searchVec;

public:
    int Main();
    void testSendToAny();
    void testSendToCol();
    void testSendToAnyThenCol();
    void testDirectHop();
    void testDirectRoute();
    void testRoutingPolicyCache();
    void debugTrace();

private:
    void setup();
    void teardown();

    void assertSrc(Client& src);
    void assertItr(DocProc& itr);
    void assertDst(Search& dst);
};

TEST_APPHOOK(Test);

int
Test::Main()
{
    TEST_INIT("messagebus_test");

    testSendToAny();          TEST_FLUSH();
    testSendToCol();          TEST_FLUSH();
    testSendToAnyThenCol();   TEST_FLUSH();
    testDirectHop();          TEST_FLUSH();
    testDirectRoute();        TEST_FLUSH();
    testRoutingPolicyCache(); TEST_FLUSH();
    debugTrace();             TEST_FLUSH();

    TEST_DONE();
}

void
Test::setup()
{
    slobrok.reset(new Slobrok());
    client.reset(new Client(*slobrok));
    dp0.reset(new DocProc("docproc/0", *slobrok));
    dp1.reset(new DocProc("docproc/1", *slobrok));
    dp2.reset(new DocProc("docproc/2", *slobrok));
    search00.reset(new Search("search/r.0/c.0", *slobrok));
    search01.reset(new Search("search/r.0/c.1", *slobrok));
    search10.reset(new Search("search/r.1/c.0", *slobrok));
    search11.reset(new Search("search/r.1/c.1", *slobrok));
    dpVec.push_back(dp0.get());
    dpVec.push_back(dp1.get());
    dpVec.push_back(dp2.get());
    searchVec.push_back(search00.get());
    searchVec.push_back(search01.get());
    searchVec.push_back(search10.get());
    searchVec.push_back(search11.get());
    ASSERT_TRUE(client->server.waitSlobrok("docproc/0/session"));
    ASSERT_TRUE(client->server.waitSlobrok("docproc/1/session"));
    ASSERT_TRUE(client->server.waitSlobrok("docproc/2/session"));
    ASSERT_TRUE(client->server.waitSlobrok("search/r.0/c.0/session"));
    ASSERT_TRUE(client->server.waitSlobrok("search/r.0/c.1/session"));
    ASSERT_TRUE(client->server.waitSlobrok("search/r.1/c.0/session"));
    ASSERT_TRUE(client->server.waitSlobrok("search/r.1/c.1/session"));
    ASSERT_TRUE(dp0->server.waitSlobrok("search/r.0/c.0/session"));
    ASSERT_TRUE(dp0->server.waitSlobrok("search/r.0/c.1/session"));
    ASSERT_TRUE(dp0->server.waitSlobrok("search/r.1/c.0/session"));
    ASSERT_TRUE(dp0->server.waitSlobrok("search/r.1/c.1/session"));
    ASSERT_TRUE(dp1->server.waitSlobrok("search/r.0/c.0/session"));
    ASSERT_TRUE(dp1->server.waitSlobrok("search/r.0/c.1/session"));
    ASSERT_TRUE(dp1->server.waitSlobrok("search/r.1/c.0/session"));
    ASSERT_TRUE(dp1->server.waitSlobrok("search/r.1/c.1/session"));
    ASSERT_TRUE(dp2->server.waitSlobrok("search/r.0/c.0/session"));
    ASSERT_TRUE(dp2->server.waitSlobrok("search/r.0/c.1/session"));
    ASSERT_TRUE(dp2->server.waitSlobrok("search/r.1/c.0/session"));
    ASSERT_TRUE(dp2->server.waitSlobrok("search/r.1/c.1/session"));
}

void Test::teardown()
{
    dpVec.clear();
    searchVec.clear();
    search11.reset();
    search10.reset();
    search01.reset();
    search00.reset();
    dp2.reset();
    dp1.reset();
    dp0.reset();
    client.reset();
    slobrok.reset();
}

void
Test::testSendToAny()
{
    setup();
    for (uint32_t i = 0; i < 300; ++i) {
        Message::UP msg(new SimpleMessage("test"));
        EXPECT_TRUE(client->session->send(std::move(msg), "DocProc").isAccepted());
    }
    EXPECT_TRUE(dp0->waitQueueSize(100));
    EXPECT_TRUE(dp1->waitQueueSize(100));
    EXPECT_TRUE(dp2->waitQueueSize(100));
    for (uint32_t i = 0; i < dpVec.size(); ++i) {
        DocProc *p = dpVec[i];
        while (p->queue.size() > 0) {
            Routable::UP msg = p->queue.dequeue(0);
            ASSERT_TRUE(msg.get() != 0);
            Reply::UP reply(new EmptyReply());
            msg->swapState(*reply);
            reply->addError(Error(ErrorCode::FATAL_ERROR, ""));
            p->session->forward(std::move(reply));
        }
    }
    EXPECT_TRUE(client->waitQueueSize(300));
    while (client->queue.size() > 0) {
        Routable::UP reply = client->queue.dequeue(0);
        ASSERT_TRUE(reply.get() != 0);
        ASSERT_TRUE(reply->isReply());
        EXPECT_TRUE(static_cast<Reply&>(*reply).getNumErrors() == 1);
    }
    teardown();
}

void
Test::testSendToCol()
{
    setup();
    ASSERT_TRUE(SimpleMessage("msg").getHash() % 2 == 0);
    for (uint32_t i = 0; i < 150; ++i) {
        Message::UP msg(new SimpleMessage("msg"));
        EXPECT_TRUE(client->session->send(std::move(msg), "Search").isAccepted());
    }
    EXPECT_TRUE(search00->waitQueueSize(150));
    EXPECT_TRUE(search01->waitQueueSize(0));
    EXPECT_TRUE(search10->waitQueueSize(150));
    EXPECT_TRUE(search11->waitQueueSize(0));
    ASSERT_TRUE(SimpleMessage("msh").getHash() % 2 == 1);
    for (uint32_t i = 0; i < 150; ++i) {
        Message::UP msg(new SimpleMessage("msh"));
        ASSERT_TRUE(client->session->send(std::move(msg), "Search").isAccepted());
    }
    EXPECT_TRUE(search00->waitQueueSize(150));
    EXPECT_TRUE(search01->waitQueueSize(150));
    EXPECT_TRUE(search10->waitQueueSize(150));
    EXPECT_TRUE(search11->waitQueueSize(150));
    for (uint32_t i = 0; i < searchVec.size(); ++i) {
        Search *s = searchVec[i];
        while (s->queue.size() > 0) {
            Routable::UP msg = s->queue.dequeue(0);
            ASSERT_TRUE(msg.get() != 0);
            Reply::UP reply(new EmptyReply());
            msg->swapState(*reply);
            s->session->reply(std::move(reply));
        }
    }
    client->waitQueueSize(300);
    FastOS_Thread::Sleep(100);
    client->waitQueueSize(300);
    while (client->queue.size() > 0) {
        Routable::UP reply = client->queue.dequeue(0);
        ASSERT_TRUE(reply.get() != 0);
        ASSERT_TRUE(reply->isReply());
        EXPECT_TRUE(static_cast<Reply&>(*reply).getNumErrors() == 0);
    }
    teardown();
}

void
Test::testSendToAnyThenCol()
{
    setup();
    ASSERT_TRUE(SimpleMessage("msg").getHash() % 2 == 0);
    for (uint32_t i = 0; i < 150; ++i) {
        Message::UP msg(new SimpleMessage("msg"));
        EXPECT_TRUE(client->session->send(std::move(msg), "Index").isAccepted());
    }
    EXPECT_TRUE(dp0->waitQueueSize(50));
    EXPECT_TRUE(dp1->waitQueueSize(50));
    EXPECT_TRUE(dp2->waitQueueSize(50));
    for (uint32_t i = 0; i < dpVec.size(); ++i) {
        DocProc *p = dpVec[i];
        while (p->queue.size() > 0) {
            Routable::UP r = p->queue.dequeue(0);
            ASSERT_TRUE(r.get() != 0);
            p->session->forward(std::move(r));
        }
    }
    EXPECT_TRUE(search00->waitQueueSize(150));
    EXPECT_TRUE(search01->waitQueueSize(0));
    EXPECT_TRUE(search10->waitQueueSize(150));
    EXPECT_TRUE(search11->waitQueueSize(0));
    ASSERT_TRUE(SimpleMessage("msh").getHash() % 2 == 1);
    for (uint32_t i = 0; i < 150; ++i) {
        Message::UP msg(new SimpleMessage("msh"));
        ASSERT_TRUE(client->session->send(std::move(msg), "Index").isAccepted());
    }
    EXPECT_TRUE(dp0->waitQueueSize(50));
    EXPECT_TRUE(dp1->waitQueueSize(50));
    EXPECT_TRUE(dp2->waitQueueSize(50));
    for (uint32_t i = 0; i < dpVec.size(); ++i) {
        DocProc *p = dpVec[i];
        while (p->queue.size() > 0) {
            Routable::UP r = p->queue.dequeue(0);
            ASSERT_TRUE(r.get() != 0);
            p->session->forward(std::move(r));
        }
    }
    EXPECT_TRUE(search00->waitQueueSize(150));
    EXPECT_TRUE(search01->waitQueueSize(150));
    EXPECT_TRUE(search10->waitQueueSize(150));
    EXPECT_TRUE(search11->waitQueueSize(150));
    for (uint32_t i = 0; i < searchVec.size(); ++i) {
        Search *s = searchVec[i];
        while (s->queue.size() > 0) {
            Routable::UP msg = s->queue.dequeue(0);
            ASSERT_TRUE(msg.get() != 0);
            Reply::UP reply(new EmptyReply());
            msg->swapState(*reply);
            s->session->reply(std::move(reply));
        }
    }
    EXPECT_TRUE(dp0->waitQueueSize(100));
    EXPECT_TRUE(dp1->waitQueueSize(100));
    EXPECT_TRUE(dp2->waitQueueSize(100));
    for (uint32_t i = 0; i < dpVec.size(); ++i) {
        DocProc *p = dpVec[i];
        while (p->queue.size() > 0) {
            Routable::UP r = p->queue.dequeue(0);
            ASSERT_TRUE(r.get() != 0);
            p->session->forward(std::move(r));
        }
    }
    client->waitQueueSize(300);
    FastOS_Thread::Sleep(100);
    client->waitQueueSize(300);
    while (client->queue.size() > 0) {
        Routable::UP reply = client->queue.dequeue(0);
        ASSERT_TRUE(reply.get() != 0);
        ASSERT_TRUE(reply->isReply());
        EXPECT_TRUE(static_cast<Reply&>(*reply).getNumErrors() == 0);
    }
    teardown();
}

void
Test::testDirectHop()
{
    setup();
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 2; col++) {
            Search* dst = searchVec[row * 2 + col];

            // Send using name.
            ASSERT_TRUE(client->session->send(
                            Message::UP(new SimpleMessage("empty")),
                            Route().addHop(vespalib::make_vespa_string("search/r.%d/c.%d/session", row, col)))
                        .isAccepted());
            assertDst(*dst);
            assertSrc(*client);

            // Send using address.
            ASSERT_TRUE(client->session->send(
                            Message::UP(new SimpleMessage("empty")),
                            Route().addHop(Hop(dst->session->getConnectionSpec().c_str())))
                        .isAccepted());
            assertDst(*dst);
            assertSrc(*client);
        }
    }
    teardown();
}

void
Test::testDirectRoute()
{
    setup();
    ASSERT_TRUE(client->session->send(
                    Message::UP(new SimpleMessage("empty")),
                    Route()
                    .addHop(Hop("docproc/0/session"))
                    .addHop(Hop(dp0->session->getConnectionSpec()))
                    .addHop(Hop("docproc/1/session"))
                    .addHop(Hop(dp1->session->getConnectionSpec()))
                    .addHop(Hop("docproc/2/session"))
                    .addHop(Hop(dp2->session->getConnectionSpec()))
                    .addHop(Hop("search/r.0/c.0/session")))
                .isAccepted());
    assertItr(*dp0);
    assertItr(*dp0);
    assertItr(*dp1);
    assertItr(*dp1);
    assertItr(*dp2);
    assertItr(*dp2);
    assertDst(*search00);
    assertItr(*dp2);
    assertItr(*dp2);
    assertItr(*dp1);
    assertItr(*dp1);
    assertItr(*dp0);
    assertItr(*dp0);
    assertSrc(*client);

    teardown();
}

void
Test::assertDst(Search& dst)
{
    ASSERT_TRUE(dst.waitQueueSize(1));
    Routable::UP msg = dst.queue.dequeue(0);
    ASSERT_TRUE(msg.get() != 0);
    dst.session->acknowledge(Message::UP(static_cast<Message*>(msg.release())));
}

void
Test::assertItr(DocProc& itr)
{
    ASSERT_TRUE(itr.waitQueueSize(1));
    Routable::UP msg = itr.queue.dequeue(0);
    ASSERT_TRUE(msg.get() != 0);
    itr.session->forward(std::move(msg));
}

void
Test::assertSrc(Client& src)
{
    ASSERT_TRUE(src.waitQueueSize(1));
    Routable::UP msg = src.queue.dequeue(0);
    ASSERT_TRUE(msg.get() != 0);
}

void
Test::testRoutingPolicyCache()
{
    setup();
    MessageBus &bus = client->server.mb;

    IRoutingPolicy::SP all = bus.getRoutingPolicy(SimpleProtocol::NAME, "All", "");
    ASSERT_TRUE(all.get() != NULL);

    IRoutingPolicy::SP ref = bus.getRoutingPolicy(SimpleProtocol::NAME, "All", "");
    ASSERT_TRUE(ref.get() != NULL);
    ASSERT_TRUE(all.get() == ref.get());

    IRoutingPolicy::SP allArg = bus.getRoutingPolicy(SimpleProtocol::NAME, "All", "Arg");
    ASSERT_TRUE(allArg.get() != NULL);
    ASSERT_TRUE(all.get() != allArg.get());

    IRoutingPolicy::SP refArg = bus.getRoutingPolicy(SimpleProtocol::NAME, "All", "Arg");
    ASSERT_TRUE(refArg.get() != NULL);
    ASSERT_TRUE(allArg.get() == refArg.get());

    teardown();
}

void
Test::debugTrace()
{
    setup();
    ASSERT_TRUE(SimpleMessage("msg").getHash() % 2 == 0);
    for (uint32_t i = 0; i < 3; ++i) {
        Message::UP msg(new SimpleMessage("msg"));
        msg->getTrace().setLevel(4 + i);
        EXPECT_TRUE(client->session->send(std::move(msg), "Index").isAccepted());
    }
    EXPECT_TRUE(dp0->waitQueueSize(1));
    EXPECT_TRUE(dp1->waitQueueSize(1));
    EXPECT_TRUE(dp2->waitQueueSize(1));
    for (uint32_t i = 0; i < dpVec.size(); ++i) {
        DocProc *p = dpVec[i];
        while (p->queue.size() > 0) {
            Routable::UP r = p->queue.dequeue(0);
            ASSERT_TRUE(r.get() != 0);
            p->session->forward(std::move(r));
        }
    }
    EXPECT_TRUE(search00->waitQueueSize(3));
    EXPECT_TRUE(search01->waitQueueSize(0));
    EXPECT_TRUE(search10->waitQueueSize(3));
    EXPECT_TRUE(search11->waitQueueSize(0));
    for (uint32_t i = 0; i < searchVec.size(); ++i) {
        Search *s = searchVec[i];
        while (s->queue.size() > 0) {
            Routable::UP msg = s->queue.dequeue(0);
            ASSERT_TRUE(msg.get() != 0);
            Reply::UP reply(new EmptyReply());
            msg->swapState(*reply);
            s->session->reply(std::move(reply));
        }
    }
    EXPECT_TRUE(dp0->waitQueueSize(1));
    EXPECT_TRUE(dp1->waitQueueSize(1));
    EXPECT_TRUE(dp2->waitQueueSize(1));
    for (uint32_t i = 0; i < dpVec.size(); ++i) {
        DocProc *p = dpVec[i];
        while (p->queue.size() > 0) {
            Routable::UP r = p->queue.dequeue(0);
            ASSERT_TRUE(r.get() != 0);
            p->session->forward(std::move(r));
        }
    }
    client->waitQueueSize(3);
    Routable::UP reply = client->queue.dequeue(0);
    fprintf(stderr, "\nTRACE DUMP(level=%d):\n%s\n\n",
            reply->getTrace().getLevel(),
            reply->getTrace().toString().c_str());
    reply = client->queue.dequeue(0);
    fprintf(stderr, "\nTRACE DUMP(level=%d):\n%s\n\n",
            reply->getTrace().getLevel(),
            reply->getTrace().toString().c_str());
    reply = client->queue.dequeue(0);
    fprintf(stderr, "\nTRACE DUMP(level=%d):\n%s\n\n",
            reply->getTrace().getLevel(),
            reply->getTrace().toString().c_str());
    teardown();
}
