#include "catch.hpp"
#include "test-helpers.h"

#include <cache.h>
#include <configcontainer.h>
#include <rss_parser.h>

using namespace newsbeuter;

TEST_CASE("cache behaves correctly") {
	configcontainer cfg;
	cache rsscache(":memory:", &cfg);
	rss_parser parser("file://data/rss.xml", &rsscache, &cfg, nullptr);
	std::shared_ptr<rss_feed> feed = parser.parse();
	REQUIRE(feed->total_item_count() == 8);
	rsscache.externalize_rssfeed(feed, false);

	SECTION("items in search result are marked as read") {
		auto search_items = rsscache.search_for_items("Botox", "");
		REQUIRE(search_items.size() == 1);
		auto item = search_items.front();
		REQUIRE(true == item->unread());

		item->set_unread(false);
		search_items.clear();

		search_items = rsscache.search_for_items("Botox", "");
		REQUIRE(search_items.size() == 1);
		auto updatedItem = search_items.front();
		REQUIRE(false == updatedItem->unread());
	}

	std::vector<std::shared_ptr<rss_feed>> feedv;
	feedv.push_back(feed);

	cfg.set_configvalue("cleanup-on-quit", "true");
	rsscache.cleanup_cache(feedv);
}

TEST_CASE("Cleaning old articles works") {
	TestHelpers::TempFile dbfile;
	std::unique_ptr<configcontainer> cfg( new configcontainer() );
	std::unique_ptr<cache> rsscache( new cache(dbfile.getPath(), cfg.get()) );
	rss_parser parser("file://data/rss.xml", rsscache.get(), cfg.get(), nullptr);
	std::shared_ptr<rss_feed> feed = parser.parse();

	/* Adding a fresh item that won't be deleted. If it survives the test, we
	 * will know that "keep-articles-days" really deletes the old articles
	 * *only* and not the whole database. */
	auto item = std::make_shared<rss_item>(rsscache.get());
	item->set_title("Test item");
	item->set_link("http://example.com/item");
	item->set_guid("http://example.com/item");
	item->set_author("Newsbeuter Testsuite");
	item->set_description("");
	item->set_pubDate(time(nullptr)); // current time
	item->set_unread(true);
	feed->add_item(item);

	rsscache->externalize_rssfeed(feed, false);

	/* Simulating a restart of Newsbeuter. */

	/* Setting "keep-articles-days" to non-zero value to trigger
	 * cache::clean_old_articles().
	 *
	 * The value of 42 days is sufficient because the items in the test feed
	 * are dating back to 2006. */
	cfg.reset( new configcontainer() );
	cfg->set_configvalue("keep-articles-days", "42");
	rsscache.reset( new cache(dbfile.getPath(), cfg.get()) );
	rss_ignores ign;
	feed = rsscache->internalize_rssfeed("file://data/rss.xml", &ign);

	/* The important part: old articles should be gone, new one remains. */
	REQUIRE(feed->items().size() == 1);
}

TEST_CASE("Last-Modified and ETag values are preserved correctly") {
	configcontainer cfg;
	cache rsscache(":memory:", &cfg);
	const auto feedurl = "file://data/rss.xml";
	rss_parser parser(feedurl, &rsscache, &cfg, nullptr);
	std::shared_ptr<rss_feed> feed = parser.parse();
	rsscache.externalize_rssfeed(feed, false);

	/* We will run this lambda on different inputs to check different
	 * situations. */
	auto test = [&](const time_t& lm_value, const std::string& etag_value) {
		time_t last_modified = lm_value;
		std::string etag = etag_value;

		rsscache.update_lastmodified(feedurl, last_modified, etag);
		/* Scrambling the value to make sure the following call changes it. */
		last_modified = 42;
		etag = "42";
		rsscache.fetch_lastmodified(feedurl, last_modified, etag);

		REQUIRE(last_modified == lm_value);
		REQUIRE(etag == etag_value);
	};

	SECTION("Only Last-Modified header was returned") {
		test(1476382350, "");
	}

	SECTION("Only ETag header was returned") {
		test(0, "1234567890");
	}

	SECTION("Both Last-Modified and ETag headers were returned") {
		test(1476382350, "1234567890");
	}
}

TEST_CASE("catchup_all marks all items read") {
	std::shared_ptr<rss_feed> feed, test_feed;

	rss_ignores ign;
	configcontainer cfg;
	cache rsscache(":memory:", &cfg);

	test_feed = std::make_shared<rss_feed>(&rsscache);
	test_feed->set_title("Test feed");
	test_feed->set_link("http://example.com/atom.xml");

	std::vector<std::string> feeds = {
		// { feed's URL, number of items in the feed }
		"file://data/rss.xml",
		"file://data/atom10_1.xml"
	};

	/* Ensure that the feeds contain expected number of items, then externalize
	 * them (put into cache). */
	for (const auto& feedurl : feeds) {
		rss_parser parser(feedurl, &rsscache, &cfg, nullptr);
		feed = parser.parse();

		test_feed->add_item(feed->items()[0]);

		rsscache.externalize_rssfeed(feed, false);
	}

	SECTION("empty feedurl") {
		INFO("All items should be marked as read.");
		rsscache.catchup_all();

		for (const auto& feedurl : feeds) {
			feed = rsscache.internalize_rssfeed(feedurl, &ign);
			for (const auto& item : feed->items()) {
				REQUIRE_FALSE(item->unread());
			}
		}
	}

	SECTION("non-empty feedurl") {
		INFO("All items with particular feedurl should be marked as read");
		rsscache.catchup_all(feeds[0]);

		INFO("First feed should all be marked read");
		feed = rsscache.internalize_rssfeed(feeds[0], &ign);
		for (const auto& item : feed->items()) {
			REQUIRE_FALSE(item->unread());
		}

		INFO("Second feed should all be marked unread");
		feed = rsscache.internalize_rssfeed(feeds[1], &ign);
		for (const auto& item : feed->items()) {
			REQUIRE(item->unread());
		}
	}

	SECTION("actual feed") {
		INFO("All items that are in the specific feed should be marked as read");
		rsscache.catchup_all(test_feed);

		/* Since test_feed contains the first item of each feed, only these two
		 * items should be marked read. */
		auto unread_items_count = [](std::shared_ptr<rss_feed>& feed) {
			unsigned int count = 0;
			for (const auto& item : feed->items()) {
				if (! item->unread()) {
					count++;
				}
			}
			return count;
		};

		feed = rsscache.internalize_rssfeed(feeds[0], &ign);
		REQUIRE(unread_items_count(feed) == 1);

		feed = rsscache.internalize_rssfeed(feeds[1], &ign);
		REQUIRE(unread_items_count(feed) == 1);
	}
}

TEST_CASE("cleanup_cache behaves as expected") {
	TestHelpers::TempFile dbfile;

	std::vector<std::string> feedurls = {
		"file://data/rss.xml",
		"file://data/atom10_1.xml"
	};

	std::vector<std::shared_ptr<rss_feed>> feeds;
	rss_ignores ign;
	std::unique_ptr<configcontainer> cfg( new configcontainer() );
	std::unique_ptr<cache> rsscache( new cache(dbfile.getPath(), cfg.get()) );
	for (const auto& url : feedurls) {
		rss_parser parser(url, rsscache.get(), cfg.get(), nullptr);
		std::shared_ptr<rss_feed> feed = parser.parse();
		feeds.push_back(feed);
		rsscache->externalize_rssfeed(feed, false);
	}

	SECTION("cleanup-on-quit set to \"no\"") {
		cfg->set_configvalue("cleanup-on-quit", "no");
		rsscache->cleanup_cache(feeds);

		cfg.reset( new configcontainer() );
		rsscache.reset( new cache(dbfile.getPath(), cfg.get()) );

		for (const auto& url : feedurls) {
			std::shared_ptr<rss_feed> feed =
				rsscache->internalize_rssfeed(url, &ign);
			REQUIRE(feed->total_item_count() != 0);
		}
	}

	SECTION("cleanup-on-quit set to \"yes\"") {
		cfg->set_configvalue("cleanup-on-quit", "yes");

		SECTION("delete-read-articles-on-quit set to \"no\"") {
			/* Drop first feed; it should now be removed from the cache, too. */
			feeds.erase(feeds.cbegin(), feeds.cbegin()+1);
			rsscache->cleanup_cache(feeds);

			cfg.reset( new configcontainer() );
			rsscache.reset( new cache(dbfile.getPath(), cfg.get()) );

			std::shared_ptr<rss_feed> feed =
				rsscache->internalize_rssfeed(feedurls[0], &ign);
			REQUIRE(feed->total_item_count() == 0);
			feed = rsscache->internalize_rssfeed(feedurls[1], &ign);
			REQUIRE(feed->total_item_count() != 0);
		}

		SECTION("delete-read-articles-on-quit set to \"yes\"") {
			cfg->set_configvalue("delete-read-articles-on-quit", "yes");
			REQUIRE(feeds[0]->total_item_count() == 8);
			feeds[0]->items()[0]->set_unread(false);
			feeds[0]->items()[1]->set_unread(false);

			rsscache->cleanup_cache(feeds);

			cfg.reset( new configcontainer() );
			rsscache.reset( new cache(dbfile.getPath(), cfg.get()) );

			std::shared_ptr<rss_feed> feed =
				rsscache->internalize_rssfeed(feedurls[0], &ign);
			REQUIRE(feed->total_item_count() == 6);
		}
	}
}

TEST_CASE("fetch_descriptions fills out feed item's descriptions") {
	configcontainer cfg;
	cache rsscache(":memory:", &cfg);
	const auto feedurl = "file://data/rss.xml";
	rss_parser parser(feedurl, &rsscache, &cfg, nullptr);
	std::shared_ptr<rss_feed> feed = parser.parse();

	rsscache.externalize_rssfeed(feed, false);

	for (auto& item : feed->items()) {
		item->set_description("your test failed!");
	}

	REQUIRE_NOTHROW(rsscache.fetch_descriptions(feed.get()));

	for (auto& item : feed->items()) {
		REQUIRE(item->description() != "your test failed!");
	}
}

TEST_CASE("get_unread_count returns number of yet unread articles") {
	TestHelpers::TempFile dbfile;
	configcontainer cfg;
	std::unique_ptr<cache> rsscache( new cache(dbfile.getPath(), &cfg) );
	std::unique_ptr<rss_parser>
		parser( new rss_parser(
					"file://data/rss.xml",
					rsscache.get(),
					&cfg,
					nullptr) );
	std::shared_ptr<rss_feed> feed = parser->parse();
	// Marking one article as read to make sure get_unread_count() really
	// counts only unread articles
	feed->items()[0]->set_unread(false);
	rsscache->externalize_rssfeed(feed, false);

	REQUIRE(rsscache->get_unread_count() == 7);

	// Let's add another article to make sure get_unread_count looks at all
	// feeds present in the cache
	parser.reset( new rss_parser(
				"file://data/atom10_1.xml",
				rsscache.get(),
				&cfg,
				nullptr) );
	feed = parser->parse();
	feed->items()[0]->set_unread(false);
	feed->items()[2]->set_unread(false);
	rsscache->externalize_rssfeed(feed, false);
	REQUIRE(rsscache->get_unread_count() == 8);

	// Lastly, let's make sure the info is indeed retrieved from the database
	// and isn't just stored in the cache object
	rsscache.reset( new cache(dbfile.getPath(), &cfg) );
	REQUIRE(rsscache->get_unread_count() == 8);
}

TEST_CASE("get_read_item_guids returns GUIDs of items that are marked read") {
	TestHelpers::TempFile dbfile;
	configcontainer cfg;
	std::unique_ptr<cache> rsscache( new cache(dbfile.getPath(), &cfg) );

	// We'll keep our own count of which GUIDs are unread
	std::unordered_set<std::string> read_guids;
	std::unique_ptr<rss_parser> parser(
		new rss_parser(
			"file://data/rss.xml",
			rsscache.get(),
			&cfg,
			nullptr) );
	std::shared_ptr<rss_feed> feed = parser->parse();

	auto mark_read = [&read_guids](std::shared_ptr<rss_item> item) {
		item->set_unread(false);
		INFO("add  " + item->guid());
		read_guids.insert(item->guid());
	};

	// This function will be used to check if the result is consistent with our
	// count
	auto check = [&read_guids](const std::vector<std::string>& result) {
		auto local = read_guids;
		REQUIRE(local.size() != 0);

		for (const auto& guid : result) {
			INFO("check " + guid);
			auto it = local.find(guid);
			REQUIRE(it != local.end());
			local.erase(it);
		}

		REQUIRE(local.size() == 0);
	};

	mark_read(feed->items()[0]);
	rsscache->externalize_rssfeed(feed, false);

	INFO("Testing on single feed");
	check(rsscache->get_read_item_guids());

	// Let's add another article to make sure get_unread_count looks at all
	// feeds present in the cache
	parser.reset( new rss_parser(
			"file://data/atom10_1.xml",
			rsscache.get(),
			&cfg,
			nullptr) );
	feed = parser->parse();
	mark_read(feed->items()[0]);
	mark_read(feed->items()[2]);

	rsscache->externalize_rssfeed(feed, false);
	INFO("Testing on two feeds");
	check(rsscache->get_read_item_guids());

	// Lastly, let's make sure the info is indeed retrieved from the database
	// and isn't just stored in the cache object
	rsscache.reset( new cache(dbfile.getPath(), &cfg) );
	INFO("Testing on two feeds with new `cache` object");
	check(rsscache->get_read_item_guids());
}
