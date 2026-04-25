#ifndef CRAWLER_H
#define CRAWLER_H

#include <stddef.h>
#include <stdint.h>

/*
 * crawler  --  Fetches financial news from Polygon.io /v2/reference/news,
 *              stores articles in Postgres (news_cache table), and builds
 *              a pre-summarized market briefing digest for the bot ensemble.
 *
 * The digest is a plain-text block (~5k-10k tokens) injected into the
 * cached system prompt so all 500 Haiku bots share the same news context.
 *
 * Polygon free tier: 5 API calls/minute.  We fetch general market news
 * rather than per-ticker to stay within rate limits.
 *
 * IPC commands (added in ipc_research.c):
 *
 *   {"cmd":"crawl_news","limit":50}
 *     -> fetches up to `limit` recent articles from Polygon, stores in DB
 *     -> {"type":"crawl_done","n_fetched":47,"n_stored":45}
 *
 *   {"cmd":"get_news_digest","max_chars":32000,"days":7}
 *     -> builds a text digest from cached articles within `days` window
 *     -> {"type":"news_digest","n_articles":45,"digest":"MARKET BRIEFING..."}
 */

/* Call once at startup after polygon_rest_init().
 * Stores the API key internally for news requests. */
void crawler_init(const char *polygon_api_key);

/*
 * Fetch recent news articles from Polygon.io and store them in news_cache.
 * `limit` controls how many articles to request (max 1000, Polygon caps it).
 * Returns number of new articles stored, or -1 on error.
 */
int crawler_fetch_news(int limit);

/*
 * Build a plain-text market briefing digest from cached articles.
 *
 * `max_chars` caps the output size (~4 chars per token, so 32000 chars
 * gives roughly 8000 tokens).
 * `days` controls the lookback window (0 = all time).
 *
 * Returns a malloc'd string the caller must free, or NULL on error.
 */
char *crawler_build_digest(int max_chars, int days);

#endif /* CRAWLER_H */
