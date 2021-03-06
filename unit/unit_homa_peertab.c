#include "homa_impl.h"
#define KSELFTEST_NOT_MAIN 1
#include "kselftest_harness.h"
#include "ccutils.h"
#include "mock.h"
#include "utils.h"

FIXTURE(homa_peertab) {
	struct homa homa;
	struct homa_sock hsk;
	struct homa_peertab peertab;
};
FIXTURE_SETUP(homa_peertab)
{
	homa_init(&self->homa);
	mock_sock_init(&self->hsk, &self->homa, 0, 0);
	homa_peertab_init(&self->peertab);
}
FIXTURE_TEARDOWN(homa_peertab)
{
	homa_peertab_destroy(&self->peertab);
	homa_sock_destroy(&self->hsk);
	homa_destroy(&self->homa);
	unit_teardown();
}

TEST_F(homa_peertab, homa_peer_find__basics)
{
	struct homa_peer *peer, *peer2;
	struct homa_metrics metrics;
	
	peer = homa_peer_find(&self->peertab, 11111, &self->hsk.inet);
	ASSERT_NE(NULL, peer);
	EXPECT_EQ(11111, peer->addr);
	EXPECT_EQ(INT_MAX, peer->unsched_cutoffs[HOMA_NUM_PRIORITIES-2]);
	EXPECT_EQ(0, peer->cutoff_version);
	
	peer2 = homa_peer_find(&self->peertab, 11111, &self->hsk.inet);
	EXPECT_EQ(peer, peer2);
	
	peer2 = homa_peer_find(&self->peertab, 22222, &self->hsk.inet);
	EXPECT_NE(peer, peer2);
	
	homa_compile_metrics(&metrics);
	EXPECT_EQ(2, metrics.peer_new_entries);
}

static struct _test_data_homa_peertab *test_data;
static struct homa_peer *conflicting_peer = NULL;
static void peer_lock_hook(void) {
	mock_spin_lock_hook = NULL;
	/* Creates a peer with the same address as the one being created
	 * by the main test function below. */
	conflicting_peer = homa_peer_find(&test_data->peertab, 444,
		&test_data->hsk.inet);
}

TEST_F(homa_peertab, homa_peertab_init__vmalloc_failed)
{
	struct homa_peertab table;
	mock_vmalloc_errors = 1;
	EXPECT_EQ(ENOMEM, -homa_peertab_init(&table));
	
	/* Make sure destroy is safe after failed init. */
	homa_peertab_destroy(&table);
}

TEST_F(homa_peertab, homa_peer_find__conflicting_creates)
{
	struct homa_peer *peer;
	
	test_data = self;
	mock_spin_lock_hook = peer_lock_hook;
	peer = homa_peer_find(&self->peertab, 444, &self->hsk.inet);
	EXPECT_NE(NULL, conflicting_peer);
	EXPECT_EQ(conflicting_peer, peer);
}

TEST_F(homa_peertab, homa_peer_find__kmalloc_error)
{
	struct homa_peer *peer;
	struct homa_metrics metrics;
	
	mock_kmalloc_errors = 1;
	peer = homa_peer_find(&self->peertab, 444, &self->hsk.inet);
	EXPECT_EQ(ENOMEM, -PTR_ERR(peer));
	
	homa_compile_metrics(&metrics);
	EXPECT_EQ(1, metrics.peer_kmalloc_errors);
}

TEST_F(homa_peertab, homa_peer_find__route_error)
{
	struct homa_peer *peer;
	struct homa_metrics metrics;
	
	mock_route_errors = 1;
	peer = homa_peer_find(&self->peertab, 444, &self->hsk.inet);
	EXPECT_EQ(EHOSTUNREACH, -PTR_ERR(peer));
	
	homa_compile_metrics(&metrics);
	EXPECT_EQ(1, metrics.peer_route_errors);
}

TEST_F(homa_peertab, homa_unsched_priority)
{
	struct homa_peer peer;
	homa_peer_set_cutoffs(&peer, INT_MAX, 0, 0, INT_MAX, 200, 100, 0, 0);
	
	EXPECT_EQ(5, homa_unsched_priority(&peer, 10));
	EXPECT_EQ(4, homa_unsched_priority(&peer, 200));
	EXPECT_EQ(3, homa_unsched_priority(&peer, 201));
}