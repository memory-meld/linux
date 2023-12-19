#include <linux/btree.h>
#include <linux/min_heap.h>
#include <linux/xxhash.h>
#include <linux/module.h>
#include <linux/perf_event.h>

#include <../../kernel/events/internal.h>

#define TRY(exp)                                                              \
	({                                                                    \
		__typeof__((exp)) __err = (exp);                              \
		if ((u64)(__err) >= (u64)(-MAX_ERRNO)) {                      \
			pr_err_ratelimited("%s:%d failed with error %lld:\n", \
					   __FILE__, __LINE__, (s64)__err);   \
			dump_stack();                                         \
			return (s64)(__err);                                  \
		}                                                             \
		__err;                                                        \
	})

struct spsc_elem {
	u32 pid, tid;
	u64 time;
	u64 addr;
	u64 weight;
	u64 phys_addr;
};

enum param_defaults {
	RING_BUFFER_PAGES = 8ul << 20 >> PAGE_SHIFT,
	SAMPLE_PERIOD = 17,
	LOAD_LATENCY_THRESHOLD = 64,
	SPSC_NELEMS = (2ul << 20) / sizeof(struct spsc_elem),
	SDS_WIDTH = 8192,
	SDS_DEPTH = 4,
	MIGRATION_NCANDIDATE = 131072,
	MIGRATION_TARGET_DRAM_ACCESS_PERCENTILE = 95,
};

unsigned long ring_buffer_pages = RING_BUFFER_PAGES;
module_param_named(ring_buffer_pages, ring_buffer_pages, ulong, 0644);
MODULE_PARM_DESC(
	ring_buffer_pages,
	"Number of pages allocated for the ring buffer, defaults to 1M");

unsigned long load_latency_sample_period = SAMPLE_PERIOD;
module_param_named(load_latency_sample_period, load_latency_sample_period,
		   ulong, 0644);
MODULE_PARM_DESC(load_latency_sample_period,
		 "Sample period for ldlat event, defaults to 17");

unsigned long load_latency_threshold = LOAD_LATENCY_THRESHOLD;
module_param_named(load_latency_threshold, load_latency_threshold, ulong, 0644);
MODULE_PARM_DESC(load_latency_threshold,
		 "Load latency threshold for ldlat event, defaults to 64");

unsigned long retired_stores_sample_period = SAMPLE_PERIOD;
module_param_named(retired_stores_sample_period, retired_stores_sample_period,
		   ulong, 0644);
MODULE_PARM_DESC(retired_stores_sample_period,
		 "Sample period for retired stores event, defaults to 17");

unsigned long streaming_decaying_sketch_width = SDS_WIDTH;
module_param_named(streaming_decaying_sketch_width,
		   streaming_decaying_sketch_width, ulong, 0644);
MODULE_PARM_DESC(streaming_decaying_sketch_width,
		 "Width for streaming decaying sketch, defaults to 8192");

unsigned long streaming_decaying_sketch_depth = SDS_DEPTH;
module_param_named(streaming_decaying_sketch_depth,
		   streaming_decaying_sketch_depth, ulong, 0644);
MODULE_PARM_DESC(streaming_decaying_sketch_depth,
		 "Depth for streaming decaying sketch, defaults to 4");

unsigned long migration_candidate_size = MIGRATION_NCANDIDATE;
module_param_named(indexable_heap_capacity, migration_candidate_size, ulong,
		   0644);
MODULE_PARM_DESC(indexable_heap_capacity,
		 "Capacity for indexable heap, defaults to 131072");

unsigned long migration_target_dram_access_percentile =
	MIGRATION_TARGET_DRAM_ACCESS_PERCENTILE;
module_param_named(migration_target_dram_access_percentile,
		   migration_target_dram_access_percentile, ulong, 0644);
MODULE_PARM_DESC(
	migration_target_dram_access_percentile,
	"Target percentile of DRAM accesses for migration, defaults to 95");

enum event_index {
	EI_READ = 0,
	// EI_WRITE = 1,
	EI_MAX,
};

enum thread_index {
	TI_POLICY,
	TI_MIGRATION,
	TI_MAX,
};

enum events {
	MEM_TRANS_RETIRED_LOAD_LATENCY = 0x01cd,
	MEM_INST_RETIRED_ALL_STORES = 0x82d0,
};

struct perf_event_attr event_attrs[EI_MAX] = {
	[EI_READ] = {
		.type = PERF_TYPE_RAW,
		.config = MEM_TRANS_RETIRED_LOAD_LATENCY,
		.config1 = LOAD_LATENCY_THRESHOLD,
		.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |
			       PERF_SAMPLE_WEIGHT | PERF_SAMPLE_PHYS_ADDR,
		.sample_period = SAMPLE_PERIOD,
		.precise_ip = 3,
		.disabled = 1,
		.exclude_kernel = 1,
		.exclude_hv = 1,
		.exclude_callchain_kernel= 1,
	},
	// [EI_WRITE] = {
	// 	.type = PERF_TYPE_RAW,
	// 	.config = MEM_INST_RETIRED_ALL_STORES,
	// 	.sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_TIME | PERF_SAMPLE_ADDR |
	// 		       PERF_SAMPLE_WEIGHT | PERF_SAMPLE_PHYS_ADDR,
	// 	.sample_period = SAMPLE_PERIOD,
	// 	.precise_ip = 3,
	// 	.disabled = 1,
	// 	.exclude_kernel = 1,
	// 	.exclude_hv = 1,
	// 	.exclude_callchain_kernel= 1,
	// },
};

void event_attrs_update_param(void)
{
	event_attrs[EI_READ].config1 = load_latency_threshold;
	event_attrs[EI_READ].sample_period = load_latency_sample_period;
	// event_attrs[EI_WRITE].sample_period = retired_stores_sample_period;
}

// clang-format off
u64 mt19937u64(void)
{
#define NN 312
#define MM 156
#define MATRIX_A 0xB5026F5AA96619E9ULL
// Most significant 33 bits
#define UM 0xFFFFFFFF80000000ULL
// Least significant 31 bits
#define LM 0x7FFFFFFFULL
	static u64 const mag01[2] = { 0ULL, MATRIX_A };
	// The array for the state vector
	static u64 mt[NN];
	// mti==NN+1 means mt[NN] is not initialized
	static int mti = NN + 1;
	if (mti >= NN) {
		// generate NN words at one time
		// if init_genrand64() has not been called,
		// a default initial seed 5489ULL is used
		if (mti == NN + 1) {
			mt[0] = 0x990124ULL;
			for (mti = 1; mti < NN; mti++) {
				mt[mti] = 6364136223846793005ULL * (mt[mti - 1] ^ (mt[mti - 1] >> 62)) + mti;
			}
		}
		for (int i = 0; i < NN - MM; i++) {
			u64 x = (mt[i] & UM) | (mt[i + 1] & LM);
			mt[i] = mt[i + MM] ^ (x >> 1) ^ mag01[x & 1ULL];
		}
		for (int i = NN - MM; i < NN - 1; i++) {
			u64 x = (mt[i] & UM) | (mt[i + 1] & LM);
			mt[i] = mt[i + (MM - NN)] ^ (x >> 1) ^ mag01[x & 1ULL];
		}
		u64 x = (mt[NN - 1] & UM) | (mt[0] & LM);
		mt[NN - 1] = mt[MM - 1] ^ (x >> 1) ^ mag01[x & 1ULL];
		mti = 0;
	}
	u64 x = mt[mti++];
	x ^= (x >> 29) & 0x5555555555555555ULL; x ^= (x << 17) & 0x71D67FFFEDA60000ULL;
	x ^= (x << 37) & 0xFFF7EEE000000000ULL; x ^= (x >> 43);
	return x;
#undef NN
#undef MM
#undef MATRIX_A
#undef UM
#undef LM
}

u64 streaming_decaying_sketch_powb(u64 exp) {
	static u64 const powers[] = {1ull,1ull,1ull,1ull,1ull,1ull,1ull,1ull,1ull,1ull,2ull,2ull,2ull,2ull,2ull,3ull,3ull,3ull,3ull,4ull,4ull,5ull,5ull,5ull,6ull,6ull,7ull,7ull,8ull,9ull,10ull,10ull,11ull,12ull,13ull,14ull,15ull,17ull,18ull,20ull,21ull,23ull,25ull,27ull,29ull,31ull,34ull,37ull,40ull,43ull,46ull,50ull,54ull,59ull,63ull,68ull,74ull,80ull,86ull,93ull,101ull,109ull,118ull,127ull,137ull,148ull,160ull,173ull,187ull,202ull,218ull,236ull,254ull,275ull,297ull,321ull,346ull,374ull,404ull,436ull,471ull,509ull,550ull,594ull,642ull,693ull,748ull,808ull,873ull,943ull,1018ull,1100ull,1188ull,1283ull,1386ull,1497ull,1616ull,1746ull,1885ull,2036ull,2199ull,2375ull,2565ull,2771ull,2992ull,3232ull,3490ull,3770ull,4071ull,4397ull,4749ull,5129ull,5539ull,5982ull,6461ull,6978ull,7536ull,8139ull,8790ull,9493ull,10252ull,11073ull,11959ull,12915ull,13949ull,15065ull,16270ull,17571ull,18977ull,20495ull,22135ull,23906ull,25818ull,27884ull,30115ull,32524ull,35126ull,37936ull,40971ull,44248ull,47788ull,51611ull,55740ull,60200ull,65016ull,70217ull,75834ull,81901ull,88453ull,95529ull,103172ull,111426ull,120340ull,129967ull,140364ull,151594ull,163721ull,176819ull,190964ull,206242ull,222741ull,240560ull,259805ull,280589ull,303037ull,327280ull,353462ull,381739ull,412278ull,445261ull,480881ull,519352ull,560900ull,605772ull,654234ull,706573ull,763099ull,824147ull,890078ull,961285ull,1038187ull,1121242ull,1210942ull,1307817ull,1412443ull,1525438ull,1647473ull,1779271ull,1921613ull,2075342ull,2241369ull,2420679ull,2614333ull,2823480ull,3049359ull,3293307ull,3556772ull,3841314ull,4148619ull,4480508ull,4838949ull,5226065ull,5644150ull,6095682ull,6583337ull,7110004ull,7678804ull,8293109ull,8956557ull,9673082ull,10446929ull,11282683ull,12185298ull,13160122ull,14212931ull,15349966ull,16577963ull,17904200ull,19336536ull,20883459ull,22554136ull,24358467ull,26307144ull,28411716ull,30684653ull,33139426ull,35790580ull,38653826ull,41746132ull,45085823ull,48692689ull,52588104ull,56795152ull,61338765ull,66245866ull,71545535ull,77269178ull,83450712ull,90126769ull,97336911ull,105123864ull,113533773ull,122616475ull,132425793ull,143019856ull,154461445ull,166818360ull,180163829ull,194576936ull,210143091ull,226954538ull,245110901ull,264719773ull,285897355ull,308769143ull,333470675ull,360148329ull,388960195ull,420077011ull,453683172ull,489977826ull,529176052ull,571510136ull,617230947ull,666609423ull,719938177ull,777533231ull,839735889ull,906914760ull,979467941ull,1057825377ull,1142451407ull,1233847519ull,1332555321ull,1439159747ull,1554292526ull,1678635929ull,1812926803ull,1957960947ull,2114597823ull,2283765649ull,2466466901ull,2663784253ull,2876886993ull,3107037953ull,3355600989ull,3624049068ull,3913972994ull,4227090833ull,4565258100ull,4930478748ull,5324917048ull,5750910412ull,6210983244ull,6707861904ull,7244490856ull,7824050125ull,8449974135ull,9125972066ull,9856049831ull,10644533818ull,11496096523ull,12415784245ull,13409046985ull,14481770744ull,15640312403ull,16891537395ull,18242860387ull,19702289218ull,21278472356ull,22980750144ull,24819210156ull,26804746968ull,28949126726ull,31265056864ull,33766261413ull,36467562326ull,39384967312ull,42535764697ull,45938625873ull,49613715943ull,53582813218ull,57869438276ull,62498993338ull,67498912805ull,72898825829ull,78730731895ull,85029190447ull,91831525683ull,99178047738ull,107112291557ull,115681274881ull,124935776872ull,134930639022ull,145725090143ull,157383097355ull,169973745143ull,183571644755ull,198257376335ull,214117966442ull,231247403758ull,249747196058ull,269726971743ull,291305129482ull,314609539841ull,339778303028ull,366960567271ull,396317412652ull,428022805665ull,462264630118ull,499245800527ull,539185464569ull,582320301735ull,628905925874ull,679218399944ull,733555871939ull,792240341694ull,855619569030ull,924069134553ull,997994665317ull,1077834238542ull,1164060977626ull,1257185855836ull,1357760724303ull,1466381582247ull,1583692108827ull,1710387477533ull,1847218475735ull,1994995953794ull,2154595630098ull,2326963280506ull,2513120342946ull,2714169970382ull,2931303568012ull,3165807853454ull,3419072481730ull,3692598280268ull,3988006142690ull,4307046634105ull,4651610364833ull,5023739194020ull,5425638329542ull,5859689395905ull,6328464547578ull,6834741711384ull,7381521048295ull,7972042732158ull,8609806150731ull,9298590642789ull,10042477894213ull,10845876125750ull,11713546215810ull,12650629913075ull,13662680306121ull,14755694730610ull,15936150309059ull,17211042333784ull,18587925720487ull,20074959778126ull,21680956560376ull,23415433085206ull,25288667732022ull,27311761150584ull,29496702042631ull,31856438206042ull,34404953262525ull,37157349523527ull,40129937485409ull,43340332484242ull,46807559082981ull,50552163809620ull,54596336914389ull,58964043867541ull,63681167376944ull,68775660767099ull,74277713628467ull,80219930718745ull,86637525176245ull,93568527190344ull,101054009365572ull,109138330114818ull,117869396524003ull,127298948245923ull,137482864105597ull,148481493234045ull,160360012692769ull,173188813708190ull,187043918804845ull,202007432309233ull,218168026893972ull,235621469045490ull,254471186569129ull,274828881494659ull,296815192014232ull,320560407375371ull,346205239965400ull,373901659162633ull,403813791895643ull,436118895247295ull,471008406867078ull,508689079416445ull,549384205769760ull,593334942231341ull,640801737609849ull,692065876618637ull,747431146748128ull,807225638487978ull,871803689567016ull,941547984732378ull,1016871823510968ull,1098221569391845ull,1186079294943193ull,1280965638538649ull,1383442889621741ull,1494118320791480ull,1613647786454799ull,1742739609371183ull,1882158778120877ull,2032731480370548ull,2195349998800192ull,2370977998704207ull,2560656238600544ull,2765508737688588ull,2986749436703675ull,3225689391639969ull,3483744542971166ull,3762444106408860ull,4063439634921569ull,4388514805715295ull,4739595990172519ull,5118763669386321ull,5528264762937227ull,5970525943972206ull,6448168019489983ull,6964021461049182ull,7521143177933117ull,8122834632167767ull,8772661402741189ull,9474474314960484ull,10232432260157324ull,11051026840969910ull,11935108988247504ull,12889917707307306ull,13921111123891892ull,15034800013803244ull,16237584014907504ull,17536590736100106ull,18939517994988116ull,20454679434587170ull,22091053789354144ull,23858338092502476ull,25767005139902676ull,27828365551094892ull,30054634795182484ull,32459005578797084ull,35055726025100852ull,37860184107108920ull,40888998835677640ull,44160118742531860ull,47692928241934410ull,51508362501289170ull,55629031501392300ull,60079354021503700ull,64885702343223990ull,70076558530681910ull,75682683213136460ull,81737297870187400ull,88276281699802380ull,95338384235786580ull,102965454974649500ull,111202691372621470ull,120098906682431200ull,129706819217025710ull,140083364754387780ull,151290033934738800ull,163393236649517920ull,176464695581479360ull,190581871227997730ull,205828420926237570ull,222294694600336580ull,240078270168363520ull,259284531781832600ull,280027294324379230ull,302429477870329600ull,326623836099956000ull,352753742987952450ull,380974042426988700ull,411451965821147800ull,444368123086839600ull,479917572933786800ull,518310978768489800ull,559775857069969000ull,604557925635566600ull,652922559686411900ull,705156364461324900ull,761568873618230900ull,822494383507689500ull,888293934188304600ull,959357448923369100ull,1036106044837238700ull,1118994528424217900ull,1208514090698155300ull,1305195217954007800ull,1409610835390328600ull,1522379702221555000ull,1644170078399279400ull,1775703684671221800ull,1917759979444919600ull,2071180777800513300ull,2236875240024554500ull,2415825259226519000ull,2609091279964641000ull,2817818582361812000ull,3043244068950757400ull,3286703594466818000ull,3549639882024164000ull,3833611072586097000ull,4140299958392985000ull,4471523955064424000ull,4829245871469578000ull,5215585541187145000ull,5632832384482117000ull,6083458975240687000ull,6570135693259942000ull,7095746548720737000ull,7663406272618397000ull,8276478774427869000ull,8938597076382099000ull,9653684842492668000ull,10425979629892082000ull,11260058000283450000ull,12160862640306127000ull,13133731651530619000ull,14184430183653069000ull,15319184598345314000ull,16544719366212940000ull,17868296915509977000ull};
	static u64 const len = ARRAY_SIZE(powers);
	return exp < len ? powers[exp] : powers[len - 1];
}
// clang-format on

struct streaming_decaying_sketch {
	u64 w, d;
	struct streaming_decaying_sketch_slot {
		u16 fingerprint, count;
	} *slots;
};

static void streaming_decaying_sketch_update_param(void)
{
	if (streaming_decaying_sketch_width == SDS_WIDTH) {
		// The original implementation has 2000 < W < 12000.
		// When giving b = 1.08, N = 10^7, ε = 2^−16 or 2^−17,
		// the error rate is within (0.01,0.05)
		// if we choose W = 7000, we have W = 7/10000 of N
		u64 spanned = 0;
		int nid;
		for_each_node_state(nid, N_MEMORY) {
			spanned += NODE_DATA(nid)->node_spanned_pages;
		}
		streaming_decaying_sketch_width = spanned * 7 / 10000;
		pr_info("%s: streaming_decaying_sketch_width=%lu\n", __func__,
			streaming_decaying_sketch_width);
	}
	// leave depth unchanged
}

static u64 streaming_decaying_sketch_hash(u64 key, u64 i)
{
	return xxh64(&key, sizeof(key), 0x25BBE08F + i);
}

static void streaming_decaying_sketch_drop(struct streaming_decaying_sketch *s)
{
	if (s->slots) {
		kvfree(s->slots);
	}
}

static int __must_check streaming_decaying_sketch_init(
	struct streaming_decaying_sketch *s, u64 w, u64 d)
{
	s->w = w;
	s->d = d;
	s->slots = kvcalloc(sizeof(struct streaming_decaying_sketch_slot),
			    d * w, GFP_KERNEL);
	if (!s->slots) {
		return -ENOMEM;
	}
	return 0;
}

static struct streaming_decaying_sketch_slot *
streaming_decaying_sketch_at(struct streaming_decaying_sketch *s, u64 i, u64 j)
{
	return &s->slots[i * s->w + j];
}

static u16 streaming_decaying_sketch_push(struct streaming_decaying_sketch *s,
					  u64 key)
{
	pr_info_ratelimited("%s: key=0x%llx\n", __func__, key);
	u16 count = 0;
	for (u64 i = 0; i < s->d; ++i) {
		u64 hash = streaming_decaying_sketch_hash(key, i);
		struct streaming_decaying_sketch_slot *slot =
			streaming_decaying_sketch_at(s, i, hash % s->w);
		u16 fingerprint = hash;
		u16 *f = &slot->fingerprint, *c = &slot->count;
		if (*f == fingerprint) {
			*c = min((u16)(*c + 1), (u16)SHRT_MAX);
		} else {
			// empty slot or decay with probability
			if (0 ==
			    mt19937u64() % streaming_decaying_sketch_powb(*c)) {
				pr_info_ratelimited(
					"%s: decaying key=0x%llx i=%llu fingerprint=0x%x count=%u\n",
					__func__, key, i, *f, *c);
				*c = *c > 0 ? *c - 1 : *c;
				if (*c == 0) {
					*f = fingerprint;
					*c = 1;
				}
			}
		}
		count = max(*c, count);
	}
	return count;
}

struct pair {
	u64 key, value;
};

static bool indexable_heap_pair_less(struct pair const *lhs,
				     struct pair const *rhs)
{
	if (lhs->key < rhs->key) {
		return true;
	} else if (lhs->key > rhs->key) {
		return false;
	} else {
		return lhs->value < rhs->value;
	}
}

static bool indexable_heap_pair_value_less(struct pair const *lhs,
					   struct pair const *rhs)
{
	return lhs->value < rhs->value;
}

static bool indexable_heap_pair_value_greater(struct pair const *lhs,
					      struct pair const *rhs)
{
	return lhs->value > rhs->value;
}

static void indexable_heap_pair_swap(struct pair *lhs, struct pair *rhs)
{
	swap(*lhs, *rhs);
}

struct indexable_heap {
	// The basic idea would be using a heap and an index table.
	// We can use linux's min_heap as the internal heap container.
	// Because it provides a customizable swap callback,
	// which allow us to update the key->pos index table.
	// The index table is a btree, because currently there is no convienient
	// in-memory array-based hash table implementation in kernel.
	// map key (u64) -> position (u64) in the heap
	struct btree_head64 index;
	// store (u64, u64) pairs
	struct min_heap heap;
	struct min_heap_callbacks heap_cbs;
};

static u64 indexable_heap_length(struct indexable_heap *h)
{
	return h->heap.nr;
}

static u64 indexable_heap_capacity(struct indexable_heap *h)
{
	return h->heap.size;
}

static int __must_check indexable_heap_index_update_or_insert(
	struct indexable_heap *h, u64 key, struct pair *p)
{
	int err = btree_update64(&h->index, key, p);
	if (err) {
		BUG_ON(err != -ENOENT);
		err = btree_insert64(&h->index, key, p, GFP_KERNEL);
	}
	return err;
}

static void indexable_heap_swap_and_update_index(struct min_heap *heap,
						 struct pair *lhs,
						 struct pair *rhs)
{
	struct indexable_heap *h =
		container_of(heap, struct indexable_heap, heap);

	swap(*lhs, *rhs);

	BUG_ON(indexable_heap_index_update_or_insert(h, lhs->key, lhs));
	BUG_ON(indexable_heap_index_update_or_insert(h, rhs->key, rhs));
}

static void indexable_heap_drop(struct indexable_heap *h)
{
	btree_destroy64(&h->index);
	kvfree(h->heap.data);
}

static void indexable_heap_update_param(void)
{
	if (migration_candidate_size == MIGRATION_NCANDIDATE) {
		// Setting candiate queue size to 10% of DRAM size
		// It will be the upper bound on the batch size of migration
		u64 dram_spanned = NODE_DATA(first_node(node_states[N_MEMORY]))
					   ->node_spanned_pages;
		migration_candidate_size = dram_spanned / 10;
		pr_info("%s: migration_candidate_size=%lu\n", __func__,
			migration_candidate_size);
	}
}
static int __must_check indexable_heap_init(struct indexable_heap *h,
					    bool min_heap, u64 cap)
{
	BUG_ON(cap == 0);
	*h = (struct indexable_heap) {
		.heap_cbs = {
			.elem_size = sizeof(struct pair),
			.less = min_heap ? (void *)indexable_heap_pair_value_less :
					   (void *)indexable_heap_pair_value_greater,
			.swp = (void *)indexable_heap_swap_and_update_index,
		},
		.heap =  {
			.data = kvcalloc(sizeof(struct pair), cap,
					 GFP_KERNEL),
			.nr = 0,
			.size = cap,
		}
	};
	btree_init64(&h->index);
	return 0;
}

static struct pair *indexable_heap_get(struct indexable_heap *h, u64 key)
{
	return btree_lookup64(&h->index, key);
}

static struct pair *indexable_heap_pop_back(struct indexable_heap *h)
{
	struct min_heap *heap = &h->heap;
	struct min_heap_callbacks *cbs = &h->heap_cbs;
	struct pair *back = min_heap_back(heap, cbs);
	if (back) {
		btree_remove64(&h->index, back->key);
		return back;
	}
	return NULL;
}

static void indexable_heap_print_debug(struct indexable_heap *h)
{
	struct min_heap *heap = &h->heap;
	struct min_heap_callbacks *cbs = &h->heap_cbs;
	pr_info("%s: cap=%d len=%d data=0x%px slots=[", __func__, heap->size,
		heap->nr, heap->data);
	u64 skip = 0;
	for (struct pair *i = min_heap_begin(heap);
	     i != min_heap_end(heap, cbs); ++i) {
		if (i->value < 5) {
			++skip;
			continue;
		}
		pr_cont(" (0x%llx, %llu),", i->key, i->value);
	}
	pr_cont("]\n");
	pr_info("%s: skipped %llu elements whose count < 5\n", __func__, skip);
}

// Track a new key or update the key's value.
// Return the old pair if the key already exists or the old pair is replaced.
// Otherwise, return (-ENOENT, -ENOENT) if a new pair is inserted,
// or (-EFBIG, -EFBIG) if the replace attempt failed.
// For a min_heap, it's used to track the top-k largest values.
// So, when we find a value larger than heap top, we should replace it.
static struct pair indexable_heap_insert(struct indexable_heap *h,
					 struct pair const *elem)
{
	pr_info_ratelimited("%s: key=0x%llx, value=%llu\n", __func__, elem->key,
			    elem->value);
	struct pair *old = indexable_heap_get(h, elem->key), ret;
	struct min_heap *heap = &h->heap;
	struct min_heap_callbacks *cbs = &h->heap_cbs;
	if (old) {
		ret = *old;
		// the value for the existing key does not change
		if (old->value == elem->value) {
			return ret;
		} else {
			// update an existing key
			old->value = elem->value;
			int pos = ((void *)old - heap->data) / cbs->elem_size;
			if (cbs->less(elem, old)) {
				// decrease the value in a min_heap, we should sift up
				min_heap_sift_up(heap, pos, cbs);
			} else {
				// icrease the value in a min_heap, we should sift down
				min_heapify(heap, pos, cbs);
			}
			return ret;
		}
	} else {
		// inserting an new key
		u64 len = indexable_heap_length(h),
		    cap = indexable_heap_capacity(h);
		if (len < cap) {
			// we have space for insertion, we can directly insert it
			min_heap_push(heap, elem, cbs);
			return (struct pair){ -ENOENT, -ENOENT };
		} else if (len == cap) {
			// If the new value is larger heap top, we should replace it,
			// so that the heap always keep the top-k largest values.
			struct pair *begin = min_heap_begin(heap);
			ret = *begin;
			if (cbs->less(begin, elem)) {
				// old top is indeed smaller
				min_heap_pop_push(heap, elem, cbs);
				btree_remove64(&h->index, ret.key);
				return ret;
			} else {
				// the replacement of top failed
				return (struct pair){ -EFBIG, -EFBIG };
			}
		} else {
			// we can never overflow the capacity
			BUG();
		}
	}
}

struct spsc {
	u64 head, tail, size;
	void *buffer;
};

static void spsc_drop(struct spsc *ch)
{
	kvfree(ch->buffer);
}

static int __must_check spsc_init(struct spsc *ch, u64 size)
{
	ch->head = ch->tail = 0;
	ch->size = size;
	ch->buffer = kvmalloc(size, GFP_KERNEL);
	if (!ch->buffer) {
		return -ENOMEM;
	}
	return 0;
}

// We will assume the buffer size is the multiple of the element size
// Producer side only modify the head pointer.
static int __must_check spsc_push(struct spsc *ch, void *buf, u64 len)
{
	u64 head = READ_ONCE(ch->head), diff = head - READ_ONCE(ch->tail);
	if (diff && diff % ch->size == 0) {
		return -ENOMEM;
	}
	memcpy(ch->buffer + head % ch->size, buf, len);
	WRITE_ONCE(ch->head, head + len);
	return 0;
}

// Consumer side only modify the tail pointer.
static int __must_check spsc_pop(struct spsc *ch, void *buf, u64 len)
{
	u64 tail = READ_ONCE(ch->tail);
	if (READ_ONCE(ch->head) == tail) {
		return -EAGAIN;
	}
	memcpy(buf, ch->buffer + tail % ch->size, len);
	WRITE_ONCE(ch->tail, tail + len);
	return 0;
}

struct placement_shared_state {
	u64 total_samples, dram_samples, pmem_samples;
};

static void placement_shared_state_count(struct placement_shared_state *state,
					 struct spsc_elem *sample)
{
	if (!sample->phys_addr) {
		return;
	}
	++state->total_samples;
	u64 pfn = PFN_DOWN(sample->phys_addr);
	bool in_dram = pfn_to_nid(pfn) == first_node(node_states[N_MEMORY]);
	if (in_dram) {
		++state->dram_samples;
	} else {
		++state->pmem_samples;
	}
}

static void placement_shared_state_merge(struct placement_shared_state *state,
					 struct placement_shared_state *diff)
{
#define __INC_ONCE(x, val)                            \
	do {                                          \
		*(volatile typeof(x) *)&(x) += (val); \
	} while (0)

	__INC_ONCE(state->total_samples, diff->total_samples);
	__INC_ONCE(state->dram_samples, diff->dram_samples);
	__INC_ONCE(state->pmem_samples, diff->pmem_samples);

#undef __INC_ONCE
}

static struct placement_shared_state
placement_shared_state_copy(struct placement_shared_state *state)
{
	struct placement_shared_state ret = {
		.total_samples = READ_ONCE(state->total_samples),
		.dram_samples = READ_ONCE(state->dram_samples),
		.pmem_samples = READ_ONCE(state->pmem_samples),
	};
	return ret;
}

struct placement {
	struct perf_event *events[NR_CPUS][EI_MAX];
	struct spsc chan[NR_CPUS][EI_MAX];
	struct task_struct *threads[TI_MAX];
	struct placement_shared_state state;
};

#define for_each_cpu_x_event(p, cpu, eidx, e)         \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)

#define for_each_cpu_x_event_ptr(p, cpu, eidx, eptr)  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu) \
		for (eidx = 0; eidx < EI_MAX; ++eidx) \
			for (eptr = &(p)->events[cpu][eidx]; eptr; eptr = NULL)

#define for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)                  \
	for (cpu = 0; cpu < num_online_cpus(); ++cpu)                         \
		for (eidx = 0; eidx < EI_MAX; ++eidx)                         \
			for (e = (p)->events[cpu][eidx]; e; e = NULL)         \
				for (; !spsc_pop(e->overflow_handler_context, \
						 &s, sizeof(s));)

static int placement_event_start(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_enable(e);
	}
	return 0;
}

static void placement_event_stop(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		perf_event_disable(e);
	}
}

static void placement_event_drop(struct placement *p)
{
	int cpu, i;
	struct perf_event *e;
	for_each_cpu_x_event(p, cpu, i, e)
	{
		struct spsc *ch = e->overflow_handler_context;
		perf_event_release_kernel(e);
		spsc_drop(ch);
	}
}

static void placement_event_overflow(struct perf_event *event,
				     struct perf_sample_data *data,
				     struct pt_regs *regs)
{
	void perf_prepare_sample(struct perf_sample_data * data,
				 struct perf_event * event,
				 struct pt_regs * regs);
	scoped_guard(rcu)
	{
		scoped_guard(irqsave)
		{
			struct spsc *ch = event->overflow_handler_context;
			perf_prepare_sample(data, event, regs);
			struct spsc_elem s = {
				.pid = data->tid_entry.pid,
				.tid = data->tid_entry.tid,
				.time = data->time,
				.addr = data->addr,
				.weight = data->weight.full,
				.phys_addr = data->phys_addr,
			};

			if (spsc_push(ch, &s, sizeof(s))) {
				pr_warn_ratelimited(
					"%s: discard sample due to ring buffer overflow\n",
					__func__);
			}
		}
	}
}

static int placement_event_init(struct placement *p)
{
	event_attrs_update_param();
	int cpu, i;
	struct perf_event **e;
	for_each_cpu_x_event_ptr(p, cpu, i, e)
	{
		struct spsc *ch = &p->chan[cpu][i];
		int err = spsc_init(ch, SPSC_NELEMS * sizeof(struct spsc_elem));
		if (err) {
			placement_event_drop(p);
			return err;
		}

		*e = perf_event_create_kernel_counter(&event_attrs[i], cpu,
						      NULL,
						      placement_event_overflow,
						      ch);
		if (IS_ERR(*e)) {
			placement_event_drop(p);
			return PTR_ERR(*e);
		}
	}

	return 0;
}

static void intel_pmu_print_debug_all(void)
{
	int cpu;
	for_each_online_cpu(cpu) {
		void perf_event_print_debug(void);
		smp_call_on_cpu(cpu, (int (*)(void *))perf_event_print_debug,
				NULL, false);
	}
}

// static int __must_check node_phys_addr_range(int nid, void **begin, void **end)
// {
// 	pg_data_t *pgdat = NODE_DATA(nid);
// 	if (!pgdat) {
// 		return -EINVAL;
// 	}
// 	u64 start_pfn = pgdat->node_start_pfn;
// 	u64 end_pfn = pgdat->node_start_pfn + pgdat->node_spanned_pages;
// 	if (begin) {
// 		*begin = (void *)PFN_PHYS(start_pfn);
// 	}
// 	if (end) {
// 		*end = (void *)PFN_PHYS(end_pfn);
// 	}
// 	return 0;
// }

static int placement_thread_fn_policy(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);
	u64 timeout = usecs_to_jiffies(1000);
	u64 interval = 10000, iter = 0, valid = 0;

	struct streaming_decaying_sketch sds;
	streaming_decaying_sketch_update_param();
	TRY(streaming_decaying_sketch_init(&sds,
					   streaming_decaying_sketch_width,
					   streaming_decaying_sketch_depth));
	struct indexable_heap demotion, promotion;
	indexable_heap_update_param();
	// This is correct, we use max_heap to keep the smallest counts for DRAM
	TRY(indexable_heap_init(&demotion, false, migration_candidate_size));
	TRY(indexable_heap_init(&promotion, true, migration_candidate_size));

	while (!kthread_should_stop()) {
		if (iter++ % interval == 0) {
			// intel_pmu_print_debug_all();
			indexable_heap_print_debug(&demotion);
			indexable_heap_print_debug(&promotion);
		}
		int cpu, eidx;
		struct perf_event *e;
		struct spsc_elem s;
		struct placement_shared_state diff = {};
		for_each_sample_from_cpu_x_event(p, cpu, eidx, e, s)
		{
			++valid;
			pr_info_ratelimited(
				"%s: got %llu-th sample: cpu=%d eidx=%d pid=%u tid=%u time=%llu addr=0x%llx weight=%llu phys_addr=0x%llx\n",
				__func__, valid, cpu, eidx, s.pid, s.tid,
				s.time, s.addr, s.weight, s.phys_addr);
			if (!s.phys_addr) {
				continue;
			}

			u64 pfn = PFN_DOWN(s.phys_addr);
			bool in_dram = pfn_to_nid(pfn) ==
				       first_node(node_states[N_MEMORY]);
			u64 count = streaming_decaying_sketch_push(&sds, pfn);
			struct pair elem = { pfn, count },
				    old = indexable_heap_insert(
					    in_dram ? &demotion : &promotion,
					    &elem);
			switch (old.key) {
			case -EFBIG:
				break;
			case -ENOENT:
				pr_info_ratelimited(
					"%s: %s candidate insert  pfn=0x%llx count=%llu\n",
					__func__,
					in_dram ? " demotion" : "promotion",
					pfn, count);
				break;
			default:
				pr_info_ratelimited(
					"%s: %s candidate %s pfn=0x%llx count=%llu old_count=%llu\n",
					__func__,
					in_dram ? " demotion" : "promotion",
					old.key == elem.key ? "update " :
							      "replace",
					pfn, count, old.value);
				break;
			}
			placement_shared_state_count(&diff, &s);
		}
		placement_shared_state_merge(&p->state, &diff);

		// give up cpu
		pr_info_ratelimited("%s: thread giving up cpu\n", __func__);
		schedule_timeout_interruptible(timeout);
	}
	return 0;
}

static int placement_thread_fn_migration(struct placement *p)
{
	pr_info("%s: thread started\n", __func__);
	u64 timeout = usecs_to_jiffies(1000);

	while (!kthread_should_stop()) {
		struct placement_shared_state state =
			placement_shared_state_copy(&p->state);
		u64 target = migration_target_dram_access_percentile;
		u64 has = state.dram_samples * 100 / (state.total_samples + 1);
		pr_info_ratelimited(
			"%s: migration starting: DRAM access percentile target=%llu has=%llu\n",
			__func__, target, has);
		if (has >= target) {
			goto sleep;
		}
sleep:
		// give up cpu
		pr_info_ratelimited("%s: thread giving up cpu\n", __func__);
		schedule_timeout_interruptible(timeout);
	}
	return 0;
}

static int placement_thread_start(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t) {
			continue;
		}
		wake_up_process(t);
	}
	return 0;
}

static void placement_thread_stop(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = p->threads[i];
		if (!t)
			continue;
		// Scynchrously stop the thread and wait for exit
		kthread_stop(t);
	}
}

static void placement_thread_drop(struct placement *p)
{
	placement_thread_stop(p);
}

static int (*placement_thread_fn[TI_MAX])(void *) = {
	[TI_POLICY] = (int (*)(void *))placement_thread_fn_policy,
	[TI_MIGRATION] = (int (*)(void *))placement_thread_fn_migration,
};
static char *placement_thread_name[TI_MAX] = {
	[TI_POLICY] = "placement_policy",
	[TI_MIGRATION] = "placement_migration",
};
// static int placement_thread_nice[TI_MAX] = {
// 	[TI_POLICY] = 1,
// 	[TI_MIGRATION] = -1,
// };

static int __must_check placement_thread_init(struct placement *p)
{
	for (int i = 0; i < TI_MAX; ++i) {
		struct task_struct *t = kthread_create(
			placement_thread_fn[i], p, placement_thread_name[i]);
		pr_info("%s: kthread_create(%s) = 0x%px\n", __func__,
			placement_thread_name[i], t);
		if (IS_ERR(t)) {
			placement_thread_drop(p);
			return PTR_ERR(t);
		} else {
			// sched_set_normal(t, placement_thread_nice[i]);
			p->threads[i] = t;
		}
	}

	return 0;
}

static void placement_drop(struct placement *p)
{
	placement_thread_drop(p);
	placement_event_drop(p);
}

static int placement_init(struct placement *p)
{
	memset(p, 0, sizeof(*p));
	TRY(placement_event_init(p));
	TRY(placement_thread_init(p));

	return 0;
}

static struct placement __global_placement;

static int init(void)
{
	struct placement *p = &__global_placement;
	TRY(placement_init(p));
	TRY(placement_thread_start(p));
	TRY(placement_event_start(p));
	return 0;
}

static void exit(void)
{
	struct placement *p = &__global_placement;
	placement_drop(p);
}

module_init(init);
module_exit(exit);
MODULE_AUTHOR("Junliang Hu <jlhu@cse.cuhk.edu.hk>");
MODULE_DESCRIPTION("Memory placement optimization module");
MODULE_LICENSE("GPL");
