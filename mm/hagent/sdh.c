#include "asm-generic/bug.h"
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/xxhash.h>
#include <linux/slab.h>
#include <linux/log2.h>
#include <linux/math.h>

#include "sdh.h"

typedef unsigned sdhcnt_t;

struct sdh {
	unsigned long w, d, k;
	// a 2d array of w x d for fingerprints
	sdhcnt_t *f;
	// a 2d array of w x d for access count
	sdhcnt_t *c;
	// a heap for storing top-k accessed address and access count
	struct rheap *h;
};

static unsigned long const SEEDS[] = {
	0x33196aa8cc858657, 0x6179cbf6b196383b, 0xa1610262ded8fa0b,
	0xd8c952cb2ef31ba9, 0xe114c80821dc3c2d, 0xa84c339b589fba0d,
	0x5fad4f73926745a7, 0x4ab127efa48fb499, 0x766edfff707a4be7,
	0xd50e23f52f5ca7a9, 0x4958c180e1b0b4cd, 0x596dd0e6afa981d1,
	0x5fb76b53d26960fd, 0x926593c357ed5b57, 0xb82d62310fdca4b5,
	0xa8dffd9f432c0941, 0x183ac9a532e05,	0xc9360c116079424d,
	0x96af9ff5b0d48419, 0x9b73fd5c6b166797, 0x41da1caf8189081f,
	0x3db6cc2ab5dd26f,  0xdb576c830463e579, 0x614028bdc177e407,
	0xb4fe2dd598d7fd1,  0x1fba31ef9b3c2fe3, 0xa9508a700af534c9,
	0x8fa0e730fb408885, 0x153cdbe1464d8ff3, 0x52df1d0c030b94ab,
	0x90466ff586985b87, 0x92d6a332fad149f7, 0x8f7fbcc8696b378b,
};

struct sdh *sdh_new(unsigned long w, unsigned long d, unsigned long k)
{
	struct sdh *ret = kzalloc(sizeof(struct sdh), GFP_KERNEL), new = {
		.w = w,
		.d = d,
		.k = k,
		.f = kvzalloc(sizeof(sdhcnt_t) * w * d, GFP_KERNEL),
		.c = kvzalloc(sizeof(sdhcnt_t) * w * d, GFP_KERNEL),
		.h = rheap_new(k),
	};
	*ret = new;
	BUG_ON(!ret);
	BUG_ON(!ret->f);
	BUG_ON(!ret->c);
	BUG_ON(!ret->h);
	return ret;
}

void sdh_drop(struct sdh *sdh)
{
	BUG_ON(!sdh);
	BUG_ON(!sdh->f);
	BUG_ON(!sdh->c);
	BUG_ON(!sdh->h);
	rheap_drop(sdh->h);
	kvfree(sdh->f);
	kvfree(sdh->c);
	kfree(sdh);
}

static unsigned sdh_fingerprint(unsigned long address)
{
	return xxh32(&address, sizeof(address), SEEDS[31]);
}

static unsigned sdh_hash(unsigned long address, unsigned long i)
{
	return xxh32(&address, sizeof(address), SEEDS[i]);
}

static sdhcnt_t *sdh_f_at(struct sdh *sdh, unsigned long i, unsigned long j)
{
	return &sdh->f[i * sdh->w + j];
}
static sdhcnt_t *sdh_c_at(struct sdh *sdh, unsigned long i, unsigned long j)
{
	return &sdh->c[i * sdh->w + j];
}

unsigned sdh_get(struct sdh *sdh, unsigned long address)
{
	unsigned fingerprint = sdh_fingerprint(address);
	unsigned count = 0;
	for (int i = 0; sdh->d; ++i) {
		unsigned j = sdh_hash(address, i) % sdh->w;
		if (*sdh_f_at(sdh, i, j) == fingerprint) {
			count = max(count, *sdh_c_at(sdh, i, j));
		}
	}
	return count;
}

void sdh_show_topk(struct sdh *sdh)
{
	pr_info("top-k (address, count):");
	rheap_show_all(sdh->h);
}

static unsigned long squares64(unsigned long ctr, unsigned long key)
{
	unsigned long t, x, y, z;
	y = x = ctr * key;
	z = y + key;
	x = x * x + y;
	x = (x >> 32) | (x << 32);
	x = x * x + z;
	x = (x >> 32) | (x << 32);
	x = x * x + y;
	x = (x >> 32) | (x << 32);
	t = x = x * x + z;
	x = (x >> 32) | (x << 32);
	return t ^ ((x * x + y) >> 32);
}

static unsigned long random(void)
{
	static unsigned long counter = 0;
	return squares64(++counter, 0xc8e4fd154ce32f6d);
}

static unsigned long powb(unsigned long exp);

unsigned sdh_add(struct sdh *sdh, unsigned long address)
{
	unsigned long topk = rheap_get(sdh->h, address);
	// pr_info("%s(sdh=%px,address=0x%lx) topk=%lu", __func__, sdh, address,
	// 	topk);
	unsigned count = 0;
	unsigned fingerprint = sdh_fingerprint(address);
	for (int i = 0; i < sdh->d; ++i) {
		unsigned j = sdh_hash(address, i) % sdh->w;
		unsigned *f = sdh_f_at(sdh, i, j), *c = sdh_c_at(sdh, i, j);
		if (*f == fingerprint) {
			unsigned long v;
			if (!topk && (rheap_peek(sdh->h, NULL, &v), *c > v)) {
				// detected fingerprint collision
				// we found a slot backing a top-k address
				// (notice the > here, it must be in the heap)
				// but the given address is not in top-k
			} else {
				*c += 1;
			}
			count = max(count, *c);
		} else {
			// fingerprint mismtach: should decay
			// for empty slots, insertion will also falls here
			if (random() % powb(*c) == 0) {
				if (*c == 0 || *c == 1) {
					*f = fingerprint;
					*c = 1;
					count = max(count, 1u);

				} else {
					*c -= 1;
				}
			}
		}
	}
	if (!topk) {
		if (!rheap_full(sdh->h)) {
			if (count) {
				// filter out the rare case that the count is 0
				rheap_push(sdh->h, address, count);
			}
		} else {
			unsigned long old_addres, old_count;
			rheap_peek(sdh->h, &old_addres, &old_count);
			if (old_count <= count) {
				// found a replacement for the heap min:
				// =: we increase count one by one
				// <: it was heap min the last time,
				//	but got replaced by a new address
				rheap_replace(sdh->h, old_addres, address,
					      count);
			} else {
				// discard rarely accessed addresses
			}
		}
	} else {
		// update the top-k that is already in the heap
		if (count) {
			rheap_update(sdh->h, address, count);
		} else {
			// delete the rare case that the count is decayed to 0
			rheap_delete(sdh->h, address);
		}
	}
	return count;
}

static unsigned long powb(unsigned long exp)
{
	static unsigned long powers[] = { 1ull,
					  1ull,
					  1ull,
					  1ull,
					  1ull,
					  1ull,
					  1ull,
					  1ull,
					  1ull,
					  1ull,
					  2ull,
					  2ull,
					  2ull,
					  2ull,
					  2ull,
					  3ull,
					  3ull,
					  3ull,
					  3ull,
					  4ull,
					  4ull,
					  5ull,
					  5ull,
					  5ull,
					  6ull,
					  6ull,
					  7ull,
					  7ull,
					  8ull,
					  9ull,
					  10ull,
					  10ull,
					  11ull,
					  12ull,
					  13ull,
					  14ull,
					  15ull,
					  17ull,
					  18ull,
					  20ull,
					  21ull,
					  23ull,
					  25ull,
					  27ull,
					  29ull,
					  31ull,
					  34ull,
					  37ull,
					  40ull,
					  43ull,
					  46ull,
					  50ull,
					  54ull,
					  59ull,
					  63ull,
					  68ull,
					  74ull,
					  80ull,
					  86ull,
					  93ull,
					  101ull,
					  109ull,
					  118ull,
					  127ull,
					  137ull,
					  148ull,
					  160ull,
					  173ull,
					  187ull,
					  202ull,
					  218ull,
					  236ull,
					  254ull,
					  275ull,
					  297ull,
					  321ull,
					  346ull,
					  374ull,
					  404ull,
					  436ull,
					  471ull,
					  509ull,
					  550ull,
					  594ull,
					  642ull,
					  693ull,
					  748ull,
					  808ull,
					  873ull,
					  943ull,
					  1018ull,
					  1100ull,
					  1188ull,
					  1283ull,
					  1386ull,
					  1497ull,
					  1616ull,
					  1746ull,
					  1885ull,
					  2036ull,
					  2199ull,
					  2375ull,
					  2565ull,
					  2771ull,
					  2992ull,
					  3232ull,
					  3490ull,
					  3770ull,
					  4071ull,
					  4397ull,
					  4749ull,
					  5129ull,
					  5539ull,
					  5982ull,
					  6461ull,
					  6978ull,
					  7536ull,
					  8139ull,
					  8790ull,
					  9493ull,
					  10252ull,
					  11073ull,
					  11959ull,
					  12915ull,
					  13949ull,
					  15065ull,
					  16270ull,
					  17571ull,
					  18977ull,
					  20495ull,
					  22135ull,
					  23906ull,
					  25818ull,
					  27884ull,
					  30115ull,
					  32524ull,
					  35126ull,
					  37936ull,
					  40971ull,
					  44248ull,
					  47788ull,
					  51611ull,
					  55740ull,
					  60200ull,
					  65016ull,
					  70217ull,
					  75834ull,
					  81901ull,
					  88453ull,
					  95529ull,
					  103172ull,
					  111426ull,
					  120340ull,
					  129967ull,
					  140364ull,
					  151594ull,
					  163721ull,
					  176819ull,
					  190964ull,
					  206242ull,
					  222741ull,
					  240560ull,
					  259805ull,
					  280589ull,
					  303037ull,
					  327280ull,
					  353462ull,
					  381739ull,
					  412278ull,
					  445261ull,
					  480881ull,
					  519352ull,
					  560900ull,
					  605772ull,
					  654234ull,
					  706573ull,
					  763099ull,
					  824147ull,
					  890078ull,
					  961285ull,
					  1038187ull,
					  1121242ull,
					  1210942ull,
					  1307817ull,
					  1412443ull,
					  1525438ull,
					  1647473ull,
					  1779271ull,
					  1921613ull,
					  2075342ull,
					  2241369ull,
					  2420679ull,
					  2614333ull,
					  2823480ull,
					  3049359ull,
					  3293307ull,
					  3556772ull,
					  3841314ull,
					  4148619ull,
					  4480508ull,
					  4838949ull,
					  5226065ull,
					  5644150ull,
					  6095682ull,
					  6583337ull,
					  7110004ull,
					  7678804ull,
					  8293109ull,
					  8956557ull,
					  9673082ull,
					  10446929ull,
					  11282683ull,
					  12185298ull,
					  13160122ull,
					  14212931ull,
					  15349966ull,
					  16577963ull,
					  17904200ull,
					  19336536ull,
					  20883459ull,
					  22554136ull,
					  24358467ull,
					  26307144ull,
					  28411716ull,
					  30684653ull,
					  33139426ull,
					  35790580ull,
					  38653826ull,
					  41746132ull,
					  45085823ull,
					  48692689ull,
					  52588104ull,
					  56795152ull,
					  61338765ull,
					  66245866ull,
					  71545535ull,
					  77269178ull,
					  83450712ull,
					  90126769ull,
					  97336911ull,
					  105123864ull,
					  113533773ull,
					  122616475ull,
					  132425793ull,
					  143019856ull,
					  154461445ull,
					  166818360ull,
					  180163829ull,
					  194576936ull,
					  210143091ull,
					  226954538ull,
					  245110901ull,
					  264719773ull,
					  285897355ull,
					  308769143ull,
					  333470675ull,
					  360148329ull,
					  388960195ull,
					  420077011ull,
					  453683172ull,
					  489977826ull,
					  529176052ull,
					  571510136ull,
					  617230947ull,
					  666609423ull,
					  719938177ull,
					  777533231ull,
					  839735889ull,
					  906914760ull,
					  979467941ull,
					  1057825377ull,
					  1142451407ull,
					  1233847519ull,
					  1332555321ull,
					  1439159747ull,
					  1554292526ull,
					  1678635929ull,
					  1812926803ull,
					  1957960947ull,
					  2114597823ull,
					  2283765649ull,
					  2466466901ull,
					  2663784253ull,
					  2876886993ull,
					  3107037953ull,
					  3355600989ull,
					  3624049068ull,
					  3913972994ull,
					  4227090833ull,
					  4565258100ull,
					  4930478748ull,
					  5324917048ull,
					  5750910412ull,
					  6210983244ull,
					  6707861904ull,
					  7244490856ull,
					  7824050125ull,
					  8449974135ull,
					  9125972066ull,
					  9856049831ull,
					  10644533818ull,
					  11496096523ull,
					  12415784245ull,
					  13409046985ull,
					  14481770744ull,
					  15640312403ull,
					  16891537395ull,
					  18242860387ull,
					  19702289218ull,
					  21278472356ull,
					  22980750144ull,
					  24819210156ull,
					  26804746968ull,
					  28949126726ull,
					  31265056864ull,
					  33766261413ull,
					  36467562326ull,
					  39384967312ull,
					  42535764697ull,
					  45938625873ull,
					  49613715943ull,
					  53582813218ull,
					  57869438276ull,
					  62498993338ull,
					  67498912805ull,
					  72898825829ull,
					  78730731895ull,
					  85029190447ull,
					  91831525683ull,
					  99178047738ull,
					  107112291557ull,
					  115681274881ull,
					  124935776872ull,
					  134930639022ull,
					  145725090143ull,
					  157383097355ull,
					  169973745143ull,
					  183571644755ull,
					  198257376335ull,
					  214117966442ull,
					  231247403758ull,
					  249747196058ull,
					  269726971743ull,
					  291305129482ull,
					  314609539841ull,
					  339778303028ull,
					  366960567271ull,
					  396317412652ull,
					  428022805665ull,
					  462264630118ull,
					  499245800527ull,
					  539185464569ull,
					  582320301735ull,
					  628905925874ull,
					  679218399944ull,
					  733555871939ull,
					  792240341694ull,
					  855619569030ull,
					  924069134553ull,
					  997994665317ull,
					  1077834238542ull,
					  1164060977626ull,
					  1257185855836ull,
					  1357760724303ull,
					  1466381582247ull,
					  1583692108827ull,
					  1710387477533ull,
					  1847218475735ull,
					  1994995953794ull,
					  2154595630098ull,
					  2326963280506ull,
					  2513120342946ull,
					  2714169970382ull,
					  2931303568012ull,
					  3165807853454ull,
					  3419072481730ull,
					  3692598280268ull,
					  3988006142690ull,
					  4307046634105ull,
					  4651610364833ull,
					  5023739194020ull,
					  5425638329542ull,
					  5859689395905ull,
					  6328464547578ull,
					  6834741711384ull,
					  7381521048295ull,
					  7972042732158ull,
					  8609806150731ull,
					  9298590642789ull,
					  10042477894213ull,
					  10845876125750ull,
					  11713546215810ull,
					  12650629913075ull,
					  13662680306121ull,
					  14755694730610ull,
					  15936150309059ull,
					  17211042333784ull,
					  18587925720487ull,
					  20074959778126ull,
					  21680956560376ull,
					  23415433085206ull,
					  25288667732022ull,
					  27311761150584ull,
					  29496702042631ull,
					  31856438206042ull,
					  34404953262525ull,
					  37157349523527ull,
					  40129937485409ull,
					  43340332484242ull,
					  46807559082981ull,
					  50552163809620ull,
					  54596336914389ull,
					  58964043867541ull,
					  63681167376944ull,
					  68775660767099ull,
					  74277713628467ull,
					  80219930718745ull,
					  86637525176245ull,
					  93568527190344ull,
					  101054009365572ull,
					  109138330114818ull,
					  117869396524003ull,
					  127298948245923ull,
					  137482864105597ull,
					  148481493234045ull,
					  160360012692769ull,
					  173188813708190ull,
					  187043918804845ull,
					  202007432309233ull,
					  218168026893972ull,
					  235621469045490ull,
					  254471186569129ull,
					  274828881494659ull,
					  296815192014232ull,
					  320560407375371ull,
					  346205239965400ull,
					  373901659162633ull,
					  403813791895643ull,
					  436118895247295ull,
					  471008406867078ull,
					  508689079416445ull,
					  549384205769760ull,
					  593334942231341ull,
					  640801737609849ull,
					  692065876618637ull,
					  747431146748128ull,
					  807225638487978ull,
					  871803689567016ull,
					  941547984732378ull,
					  1016871823510968ull,
					  1098221569391845ull,
					  1186079294943193ull,
					  1280965638538649ull,
					  1383442889621741ull,
					  1494118320791480ull,
					  1613647786454799ull,
					  1742739609371183ull,
					  1882158778120877ull,
					  2032731480370548ull,
					  2195349998800192ull,
					  2370977998704207ull,
					  2560656238600544ull,
					  2765508737688588ull,
					  2986749436703675ull,
					  3225689391639969ull,
					  3483744542971166ull,
					  3762444106408860ull,
					  4063439634921569ull,
					  4388514805715295ull,
					  4739595990172519ull,
					  5118763669386321ull,
					  5528264762937227ull,
					  5970525943972206ull,
					  6448168019489983ull,
					  6964021461049182ull,
					  7521143177933117ull,
					  8122834632167767ull,
					  8772661402741189ull,
					  9474474314960484ull,
					  10232432260157324ull,
					  11051026840969910ull,
					  11935108988247504ull,
					  12889917707307306ull,
					  13921111123891892ull,
					  15034800013803244ull,
					  16237584014907504ull,
					  17536590736100106ull,
					  18939517994988116ull,
					  20454679434587168ull,
					  22091053789354144ull,
					  23858338092502476ull,
					  25767005139902676ull,
					  27828365551094892ull,
					  30054634795182484ull,
					  32459005578797084ull,
					  35055726025100852ull,
					  37860184107108920ull,
					  40888998835677640ull,
					  44160118742531856ull,
					  47692928241934408ull,
					  51508362501289168ull,
					  55629031501392304ull,
					  60079354021503696ull,
					  64885702343223992ull,
					  70076558530681912ull,
					  75682683213136464ull,
					  81737297870187392ull,
					  88276281699802384ull,
					  95338384235786576ull,
					  102965454974649504ull,
					  111202691372621472ull,
					  120098906682431200ull,
					  129706819217025712ull,
					  140083364754387776ull,
					  151290033934738816ull,
					  163393236649517920ull,
					  176464695581479360ull,
					  190581871227997728ull,
					  205828420926237568ull,
					  222294694600336576ull,
					  240078270168363520ull,
					  259284531781832608ull,
					  280027294324379232ull,
					  302429477870329600ull,
					  326623836099955968ull,
					  352753742987952448ull,
					  380974042426988672ull,
					  411451965821147776ull,
					  444368123086839616ull,
					  479917572933786816ull,
					  518310978768489792ull,
					  559775857069969024ull,
					  604557925635566592ull,
					  652922559686411904ull,
					  705156364461324928ull,
					  761568873618230912ull,
					  822494383507689472ull,
					  888293934188304640ull,
					  959357448923369088ull,
					  1036106044837238656ull,
					  1118994528424217856ull,
					  1208514090698155264ull,
					  1305195217954007808ull,
					  1409610835390328576ull,
					  1522379702221554944ull,
					  1644170078399279360ull,
					  1775703684671221760ull,
					  1917759979444919552ull,
					  2071180777800513280ull,
					  2236875240024554496ull,
					  2415825259226519040ull,
					  2609091279964640768ull,
					  2817818582361811968ull,
					  3043244068950757376ull,
					  3286703594466818048ull,
					  3549639882024163840ull,
					  3833611072586097152ull,
					  4140299958392985088ull,
					  4471523955064423936ull,
					  4829245871469578240ull,
					  5215585541187144704ull,
					  5632832384482116608ull,
					  6083458975240686592ull,
					  6570135693259941888ull,
					  7095746548720737280ull,
					  7663406272618396672ull,
					  8276478774427869184ull,
					  8938597076382099456ull,
					  9653684842492667904ull,
					  10425979629892081664ull,
					  11260058000283449344ull,
					  12160862640306126848ull,
					  13133731651530618880ull,
					  14184430183653068800ull,
					  15319184598345314304ull,
					  16544719366212939776ull,
					  17868296915509977088ull };
	return exp < ARRAY_SIZE(powers) ? powers[exp] : ULONG_MAX;
}
