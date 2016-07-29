#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>

#include "indexer/config.h" /* for MAX_CORPUS_FILE_SZ */
#include "indices/indices.h"
#include "codec/codec.h"
#include "mem-index/mem-posting.h"
#include "txt-seg/config.h"
#include "txt-seg/txt-seg.h"
#include "txt-seg/lex.h"
#include "postmerge.h"
#include "bm25-score.h"
#include "proximity.h"
#include "rank.h"
#include "config.h"

#define MIN(A, B) ((A) < (B) ? (A) : (B))

struct term_extra_score_arg {
	void                    *term_index;
	struct BM25_term_i_args *bm25args;
	ranked_results_t        *rk_res;
	prox_input_t            *prox_in;
};

void *term_posting_cur_item_wrap(void *posting)
{
	return (void*)term_posting_cur_item_with_pos(posting);
}

uint64_t term_posting_cur_item_id_wrap(void *item)
{
	doc_id_t doc_id;
	doc_id = ((struct term_posting_item*)item)->doc_id;
	return (uint64_t)doc_id;
}

bool term_posting_jump_wrap(void *posting, uint64_t to_id)
{
	bool succ;

	/* because uint64_t value can be greater than doc_id_t,
	 * we need a wrapper function to safe-guard from
	 * calling term_posting_jump with illegal argument. */
	if (to_id >= UINT_MAX)
		succ = 0;
	else
		succ = term_posting_jump(posting, (doc_id_t)to_id);

	return succ;
}

struct rank_hit *new_hit(struct postmerge *pm, doc_id_t docID,
                         float score, uint32_t n_save_occurs)
{
	uint32_t i;
	struct term_posting_item *pip /* posting item with positions */;
	struct rank_hit *hit;
	position_t *pos_arr;
	position_t *occurs;

	hit = malloc(sizeof(struct rank_hit));
	hit->docID = docID;
	hit->score = score;
	hit->n_occurs = n_save_occurs;
	hit->occurs = occurs = malloc(sizeof(position_t) * n_save_occurs);

	for (i = 0; i < pm->n_postings; i++)
		if (pm->curIDs[i] == docID) {
			pip = pm->cur_pos_item[i];
			pos_arr = TERM_POSTING_ITEM_POSITIONS(pip);

			if (n_save_occurs >= pip->tf) {
				memcpy(occurs, pos_arr, pip->tf * sizeof(position_t));
				occurs += pip->tf;
				n_save_occurs -= pip->tf;
			} else {
				memcpy(occurs, pos_arr, n_save_occurs * sizeof(position_t));
				break;
			}
		}

	return hit;
}

void
term_posting_on_merge(uint64_t cur_min, struct postmerge* pm,
                      void* extra_args)
{
	uint32_t i;
	float tot_score, bm25_score = 0.f;

#ifdef ENABLE_PROXIMITY_SEARCH
	uint32_t j = 0;
	float prox_score;
#endif

	doc_id_t docID = cur_min;
	uint32_t n_tot_occurs = 0;

	P_CAST(tes_arg, struct term_extra_score_arg, extra_args);
	float doclen = (float)term_index_get_docLen(tes_arg->term_index, docID);
	struct term_posting_item *pip /* posting item with positions */;

	for (i = 0; i < pm->n_postings; i++)
		if (pm->curIDs[i] == cur_min) {
			//printf("merge docID#%lu from posting[%d]\n", cur_min, i);
			pip = pm->cur_pos_item[i];
			n_tot_occurs += pip->tf;

//			{ /* print position array */
//				int j;
//				position_t *pos_arr = TERM_POSTING_ITEM_POSITIONS(pip);
//				for (j = 0; j < pip->tf; j++) {
//					printf("%u-", pos_arr[j]);
//				}
//				printf("\n");
//			}

#ifdef ENABLE_PROXIMITY_SEARCH
			{
				/* set proximity input */
				position_t *pos_arr = TERM_POSTING_ITEM_POSITIONS(pip);
				prox_set_input(tes_arg->prox_in + j, pos_arr, pip->tf);
				j++;
			}
#endif

			bm25_score += BM25_term_i_score(tes_arg->bm25args, i,
			                                pip->tf, doclen);
		}

#ifdef ENABLE_PROXIMITY_SEARCH
	/* calculate overall score considering proximity. */
	prox_score = prox_calc_score(prox_min_dist(tes_arg->prox_in, j));
	tot_score = bm25_score + prox_score;
#else
	tot_score = bm25_score;
#endif

//	printf("BM25 score = %f.\n", bm25_score);
//	printf("proximity score = %f.\n", prox_score);
//	printf("(total score: %f)\n", tot_score);

	if (!priority_Q_full(tes_arg->rk_res) ||
	    tot_score > priority_Q_min_score(tes_arg->rk_res)) {

		struct rank_hit *hit = new_hit(
			pm, docID, tot_score,
			MIN(MAX_HIGHLIGHT_OCCURS, n_tot_occurs)
		);
		priority_Q_add_or_replace(tes_arg->rk_res, hit);
	}
}

static char
*get_blob_string(blob_index_t bi, doc_id_t docID, bool gz, size_t *sz)
{
	struct codec   codec = {CODEC_GZ, NULL};
	static char    text[MAX_CORPUS_FILE_SZ + 1];
	size_t         blob_sz, text_sz;
	char          *blob_out = NULL;

	blob_sz = blob_index_read(bi, docID, (void **)&blob_out);

	if (blob_out) {
		if (gz) {
			text_sz = codec_decompress(&codec, blob_out, blob_sz,
					text, MAX_CORPUS_FILE_SZ);
			text[text_sz] = '\0';
			*sz = text_sz;
		} else {
			memcpy(text, blob_out, blob_sz);
			text[blob_sz] = '\0';
			*sz = blob_sz;
		}

		blob_free(blob_out);
		return text;
	}

	fprintf(stderr, "error: get_blob_string().\n");
	*sz = 0;
	return NULL;
}

static void bubble_sort(position_t *arr, uint32_t n)
{
	uint32_t i, j;
	position_t tmp;

	for (i = 0; i < n; i++)
		for (j = i; j < n; j++)
			if (arr[i] > arr[j]) {
				tmp = arr[i];
				arr[i] = arr[j];
				arr[j] = tmp;
			}
}

typedef void (*seg_it_callbk)(char*, uint32_t, size_t, void*);

struct highlighter_arg {
	position_t *pos_arr;
	uint32_t    pos_arr_now, pos_arr_sz;
	uint32_t    cur_lex_pos;
};

struct seg_it_args {
	uint32_t      slice_offset;
	seg_it_callbk fun;
	void         *arg;
};

LIST_DEF_FREE_FUN(txt_seg_li_release, struct text_seg,
                  ln, free(p));

static void
print_seg(char *mb_str, uint32_t offset, size_t sz, void *arg)
{
	printf("`%s' [%u, %lu]\n", mb_str, offset, sz);
}

static void
add_highlight_seg(char *mb_str, uint32_t offset, size_t sz, void *arg)
{
	P_CAST(ha, struct highlighter_arg, arg);

	if (ha->pos_arr_now == ha->pos_arr_sz) {
		return;
	} else if (ha->cur_lex_pos == ha->pos_arr[ha->pos_arr_now]) {
		print_seg(mb_str, offset, sz, NULL);
		ha->pos_arr_now ++;
	}

	ha->cur_lex_pos ++;
}

static LIST_IT_CALLBK(seg_iteration)
{
	LIST_OBJ(struct text_seg, seg, ln);
	P_CAST(sia, struct seg_it_args, pa_extra);

	/* adjust offset relatively to file */
	seg->offset += sia->slice_offset;

	/* call the callback function */
	sia->fun(seg->str, seg->offset, seg->n_bytes, sia->arg);

	LIST_GO_OVER;
}

static void
foreach_seg(struct lex_slice *slice, seg_it_callbk fun, void *arg)
{
	size_t str_sz = strlen(slice->mb_str);
	list   li     = LIST_NULL;
	struct seg_it_args sia = {slice->offset, fun, arg};

	switch (slice->type) {
	case LEX_SLICE_TYPE_MATH_SEG:
		fun(slice->mb_str, slice->offset, str_sz, arg);
		break;

	case LEX_SLICE_TYPE_MIX_SEG:
		li = text_segment(slice->mb_str);
		list_foreach(&li, &seg_iteration, &sia);
		txt_seg_li_release(&li);

		break;

	case LEX_SLICE_TYPE_ENG_SEG:
		fun(slice->mb_str, slice->offset, str_sz, arg);
		break;

	default:
		assert(0);
	}
}

static struct highlighter_arg hi_arg;

void token_offset_lex_handler(struct lex_slice *slice)
{
	//foreach_seg(slice, &print_seg, NULL);
	foreach_seg(slice, &add_highlight_seg, &hi_arg);
}

void print_res_item(struct rank_hit* hit, uint32_t cnt, void* arg)
{
	P_CAST(indices, struct indices, arg);
	uint32_t i;
	char    *str;
	size_t   str_sz;
	FILE    *str_fh;

	printf("result#%u: doc#%u score=%.3f\n", cnt, hit->docID, hit->score);

	bubble_sort(hit->occurs, hit->n_occurs);

	printf("occurs: ");
	for (i = 0; i < hit->n_occurs; i++)
		printf("%u ", hit->occurs[i]);
	printf("\n");

	/* print URL */
	str = get_blob_string(indices->url_bi, hit->docID, 0, &str_sz);
	printf("URL: %s" "\n\n", str);

	/* print document text */
	str = get_blob_string(indices->txt_bi, hit->docID, 1, &str_sz);
	//printf("%s" "\n--------\n\n", str);

	/* prepare highlighter arguments */
	hi_arg.pos_arr = hit->occurs;
	hi_arg.pos_arr_now = 0;
	hi_arg.pos_arr_sz = hit->n_occurs;
	hi_arg.cur_lex_pos = 0;

	/* invoke lexer */
	str_fh = fmemopen((void *)str, str_sz, "r");
	lex_eng_file(str_fh);
	fclose(str_fh);

	printf("\n");
}

uint32_t
print_all_rank_res(ranked_results_t *rk_res, struct indices *indices)
{
	struct rank_window win;
	uint32_t tot_pages, page = 0;

	do {
		win = rank_window_calc(rk_res, page, DEFAULT_RES_PER_PAGE, &tot_pages);
		if (win.to > 0) {
			printf("page#%u (from %u to %u):\n",
			       page + 1, win.from, win.to);
			rank_window_foreach(&win, &print_res_item, indices);
			page ++;
		}
	} while (page < tot_pages);

	return tot_pages;
}

static ranked_results_t
test_term_search(void *ti, enum postmerge_op op,
                 char (*terms)[MAX_TERM_BYTES], uint32_t n_terms)
{
	uint32_t                     i;
	void                        *posting;
	term_id_t                    term_id;
	struct term_extra_score_arg  tes_arg;
	ranked_results_t             rk_res;
	uint32_t                     docN, df;
	struct BM25_term_i_args      bm25args;
	struct postmerge             pm;
	struct postmerge_callbks     mem_calls, disk_calls, *calls = NULL;

	/* testing in-memory posting */
	struct mem_posting *fork_posting;
	void *term_posting;

	const term_id_t forked_term_id = 1;
	printf("caching posting list[%u] of term `%s'...\n", forked_term_id,
	       term_lookup_r(ti, forked_term_id));

	term_posting = term_index_get_posting(ti, forked_term_id);
	fork_posting = postcache_fork_term_posting(term_posting);

	printf("forked posting list: \n");
	mem_posting_print_info(fork_posting);
	printf("\n");

	/* clear post merge structure */
	postmerge_posts_clear(&pm);

	/*
	 * prepare term posting list merge callbacks.
	 */
	disk_calls.start  = &term_posting_start;
	disk_calls.finish = &term_posting_finish;
	disk_calls.jump   = &term_posting_jump_wrap;
	disk_calls.next   = &term_posting_next;
	disk_calls.now    = &term_posting_cur_item_wrap;
	disk_calls.now_id = &term_posting_cur_item_id_wrap;

	mem_calls.start  = &mem_posting_start;
	mem_calls.finish = &mem_posting_finish;
	mem_calls.jump   = &mem_posting_jump;
	mem_calls.next   = &mem_posting_next;
	mem_calls.now    = &mem_posting_cur_item;
	mem_calls.now_id = &mem_posting_cur_item_id;

	/*
	 * for each term posting list, pre-calculate some scoring
	 * parameters and find associated posting list.
	 */
	docN = term_index_get_docN(ti);

	for (i = 0; i < n_terms; i++) {
		term_id = term_lookup(ti, terms[i]);
		if (term_id != 0) {

			df = term_index_get_df(ti, term_id);
			bm25args.idf[i] = BM25_idf((float)df, (float)docN);

			printf("term `%s' ID = %u, df[%d] = %u.\n",
			       terms[i], term_id, i, df);

			if (forked_term_id == term_id) {
				posting = fork_posting;
				calls = &mem_calls;
				printf("using cached posting list.\n");
			} else {
				posting = term_index_get_posting(ti, term_id);
				calls = &disk_calls;
				printf("using on-disk posting list.\n");
			}
		} else {
			posting = NULL;
			calls = NULL;

			df = 0;
			bm25args.idf[i] = BM25_idf((float)df, (float)docN);

			printf("term `%s' not found, df[%d] = %u.\n",
			       terms[i], i, df);
		}

		postmerge_posts_add(&pm, posting, calls, NULL);
	}

	/*
	 * prepare some scoring parameters.
	 */
	bm25args.n_postings = i;
	bm25args.avgDocLen = (float)term_index_get_avgDocLen(ti);
	bm25args.b  = BM25_DEFAULT_B;
	bm25args.k1 = BM25_DEFAULT_K1;
	bm25args.frac_b_avgDocLen = BM25_DEFAULT_K1 / bm25args.avgDocLen;

	printf("BM25 arguments:\n");
	BM25_term_i_args_print(&bm25args);

	/*
	 * initialize ranking set.
	 */
	priority_Q_init(&rk_res, RANK_SET_DEFAULT_VOL);

	/*
	 * merge extra arguments.
	 */
	tes_arg.term_index = ti;
	tes_arg.bm25args = &bm25args;
	tes_arg.rk_res = &rk_res;
	tes_arg.prox_in = malloc(sizeof(prox_input_t) * pm.n_postings);

	/*
	 * merge and score.
	 */
	printf("start merging...\n");

	/*
	 * pause and continue on key press to have an idea
	 * of how long the actual search process takes.
	 */
	printf("Press Enter to Continue");
	while(getchar() != '\n');

	/* posting merge */
	if (!posting_merge(&pm, op, &term_posting_on_merge, &tes_arg))
		fprintf(stderr, "posting merge operation undefined.\n");

	/* free proximity pointer array */
	free(tes_arg.prox_in);

	/* free test in-memory posting */
	mem_posting_free(fork_posting);

	/*
	 * rank top K hits.
	 */
	priority_Q_sort(&rk_res);

	return rk_res;
}

int main(int argc, char *argv[])
{
	struct indices          indices;
	int                     opt;
	enum postmerge_op       op = POSTMERGE_OP_AND;

	char       query[MAX_MERGE_POSTINGS][MAX_TERM_BYTES];
	uint32_t   i, n_queries = 0;

	char      *index_path = NULL;

	uint32_t           res_pages;
	ranked_results_t   results;

	/* open text segmentation dictionary */
	printf("opening dictionary...\n");
	text_segment_init("");

	while ((opt = getopt(argc, argv, "hp:t:o:")) != -1) {
		switch (opt) {
		case 'h':
			printf("DESCRIPTION:\n");
			printf("test for merge postings.\n");
			printf("\n");
			printf("USAGE:\n");
			printf("%s -h | -p <index path> | -t <term> | -o <op>\n", argv[0]);
			printf("\n");
			printf("EXAMPLE:\n");
			printf("%s -p ./tmp -t 'nick ' -t 'wilde' -o OR\n", argv[0]);
			printf("%s -p ./tmp -t 'give ' -t 'up' -t 'dream' -o AND\n", argv[0]);
			goto exit;

		case 'p':
			index_path = strdup(optarg);
			break;

		case 't':
			strcpy(query[n_queries ++], optarg);
			break;

		case 'o':
		{
			if (strcmp(optarg, "AND") == 0)
				op = POSTMERGE_OP_AND;
			else if (strcmp(optarg, "OR") == 0)
				op = POSTMERGE_OP_OR;
			else
				op = POSTMERGE_OP_UNDEF;

			break;
		}

		default:
			printf("bad argument(s). \n");
			goto exit;
		}
	}

	/*
	 * check program arguments.
	 */
	if (index_path == NULL || n_queries == 0) {
		printf("not enough arguments.\n");
		goto exit;
	}

	/*
	 * print program arguments.
	 */
	printf("index path: %s\n", index_path);
	printf("query: ");
	for (i = 0; i < n_queries; i++) {
		printf("`%s'", query[i]);
		if (i + 1 != n_queries)
			printf(", ");
		else
			printf(".");
	}
	printf("\n");

	/*
	 * open indices.
	 */
	printf("opening index...\n");
	if (indices_open(&indices, index_path, INDICES_OPEN_RD)) {
		printf("indices open failed.\n");
		goto exit;
	}

	/*
	 * perform search.
	 */
	printf("searching terms...\n");
	results = test_term_search(indices.ti, op, query, n_queries);

	/*
	 * register lex handler.
	 */
	g_lex_handler = token_offset_lex_handler;

	/*
	 * print ranked search results by page number.
	 */
	res_pages = print_all_rank_res(&results, &indices);
	printf("result(s): %u pages.\n", res_pages);

	free_ranked_results(&results);

	/*
	 * close indices.
	 */
	printf("closing index...\n");
	indices_close(&indices);

exit:
	text_segment_free();

	if (index_path)
		free(index_path);

	return 0;
}
