# Literatür taraması: NFSU2 macOS portu (2026-10-20)

Bu belge, projenin yaptığı işin (x86-32 Win32 oyununu C'ye statik lift edip ARM64
macOS'ta yerel çalıştırmak; D3D9 → Metal; ölçüm ve grafik iyileştirmeleri) akademik
ve yarı-akademik literatürdeki karşılıklarını toplar. Amaç iki yönlü: (1) projede
"kendimiz bulduk" dediğimiz şeylerin literatürde nerede durduğunu görmek, (2) henüz
denemediğimiz ve işe yarayabilecek teknikleri çıkarmak.

**Yöntem.** Konu başlığı başına web araması; her kaynak için açık erişimli bir
kopya (arXiv, USENIX/NDSS gibi açık bildiriler, yazar sayfası, kurum deposu) arandı.
Bağlantılar 2026-10-20'de bu makineden `curl` ile denendi.

**İşaretler**

- ✅ açık PDF/HTML, bağlantı çalışıyor
- 🔒 açık kopya bulunamadı veya bu makineden erişilemedi (DOI/özet sayfası verildi)
- 🧭 akademik değil (blog, sunum, araç belgesi), ama literatürde boşluk olduğu için eklendi
- ⭐ projeye doğrudan dokunan, önce okunması önerilen kaynak

Makale özetleri özet sayfalarından ve (⭐ işaretlilerin bir kısmında) tam metinden
çıkarıldı. Tam metnini okumadığım kaynaklardaki sayılar makalenin kendi iddiasıdır.

---

## 0. Kısa özet: önce okunacak 12 kaynak

| # | Kaynak | Neden |
|---|---|---|
| 1 | ⭐ Arancini (ASPLOS 2026) | x86 → Arm **hibrit** (statik + dinamik) çevirici; bizim `genexec` yaklaşımının akademik karşılığı; ölü bayrak eleme bulgusu bizim ölçümümüzle aynı |
| 2 | ⭐ MirrorBall / "No Bit Left Behind" (arXiv, Eyl 2026) | Tamamen statik, sezgisiz lift; işaretçi/sabit sorununu kaba kuvvetle çözüyor; bizim reloc yaklaşımıyla karşılaştırma için ideal |
| 3 | ⭐ Elevator (arXiv, May 2026) | Aynı grup, x86-64 → AArch64 tamamen statik çapraz-ISA çeviri |
| 4 | ⭐ Lasagne (PLDI 2022) | Statik x86 → Arm çeviri, LLVM tabanlı; bellek modeli |
| 5 | ⭐ BinRec (EuroSys 2020) | Dinamik izden lift + yeniden derleme; "sezgisiz" çizginin dinamik ucu |
| 6 | ⭐ SoK x86 disassembly (S&P 2021) ve Andriesse vd. (USENIX Sec 2016) | Fonksiyon/jump table/kod-veri ayrımının sistematik değerlendirmesi |
| 7 | ⭐ Padding Matters (CODASPY 2025) | **PE** dosyalarında fonksiyon tespiti; Windows ikilileri için az sayıdaki çalışmadan biri |
| 8 | ⭐ Testing CPU emulators / Hi-Fi tests for Lo-Fi emulators / MeanDiff | Bizim Rosetta diferansiyel fuzzer'ımızın akademik akrabaları |
| 9 | ⭐ Monniaux, "The pitfalls of verifying floating-point computations" | x87 80-bit ↔ 64-bit double farkının kanonik anlatımı |
| 10 | ⭐ STACK (SOSP 2013) + Cerberus/provenance (POPL 2019) | Üretilen C'deki UB tuzakları (bizim katı takma ad hatasının genel hali) |
| 11 | ⭐ HALucinator (USENIX Sec 2020) | "Kütüphane fonksiyonunu tanı, yerel karşılığıyla değiştir" = bizim D3DX HLE'nin ürün yazılımı karşılığı |
| 12 | ⭐ GraphicsFuzz (OOPSLA 2017) | Shader derleyicilerini metamorfik testle doğrulamak; bizim vs/ps → MSL çevirimize uygulanabilir |

---

## 1. Statik ikili çeviri ve yeniden derleme (çekirdek)

Projenin ana yöntemi. Literatürde iki çizgi var: (a) IR'a (çoğunlukla LLVM) lift edip
yeniden derleyen "binary lifter"lar, (b) baştan bir ISA'dan diğerine çeviren
"static binary translator"lar. Bizim lifter'ımız C'ye lift edip clang'a veriyor, yani
(a) çizgisinin "C hedefli" bir varyantı.

### Klasikler

- **UQBT: Adaptable Binary Translation at Low Cost.** Cifuentes, Van Emmerik. *IEEE
  Computer* 33(3), 2000. DOI 10.1109/2.825697. 🔒
  Makine tanımlarından üretilen, kaynak ve hedef ISA'sı değiştirilebilen statik
  çevirici. Statik çevirinin temel sorunlarını (dolaylı dallanma, kod keşfi) ilk
  sistematik ele alan iş.
- **DIGITAL FX!32: A Profile-Directed Binary Translator.** Chernoff, Herdeg, Hookway ve ark.
  *IEEE Micro* 1998. ✅ [PDF](https://web.stanford.edu/class/cs343/resources/fx32.pdf)
  Windows NT'de x86 → Alpha. Önce yorumlayıcı + profil, sonra arka planda statik
  çeviri. **Win32 uygulamalarını başka bir ISA'da çalıştırmanın ilk ticari örneği;**
  `CreateProcess` yakalama gibi Win32'ye özgü sorunları tartışıyor.
- **Binary Translation Using Peephole Superoptimizers.** Bansal, Aiken. OSDI 2008. ✅
  [PDF](https://db.usenix.org/legacy/events/osdi08/tech/full_papers/bansal/bansal.pdf)
  PowerPC → x86 çeviri kurallarını süperoptimizasyonla otomatik öğreniyor; yerel
  derleyicinin medyan %67'si performans. Kuralları elle yazmak yerine öğrenme fikri,
  bizim komut-komut lift şablonlarımıza alternatif.
- **LLBT: An LLVM-based Static Binary Translator.** Shen, Chen, Hsu, Yang. CASES 2012;
  genişletilmiş hali *A Retargetable Static Binary Translator for the ARM
  Architecture*, ACM TACO 11(2), 2014, DOI 10.1145/2629335. 🔒
  ARM → LLVM IR → çeşitli hedefler. **Tüm ikiliyi tek fonksiyon gibi ele alıp misafir
  yazmaçlarını LLVM'in küresel yazmaç tahsisine bırakıyor** (QEMU'dan 6×, HQEMU'dan
  2.3× hızlı). Bizim `CPU *c` yapısını fonksiyonlar arası taşıma maliyetimizle
  (bkz. `LIFTER_VERIMLILIK.md` §3 çağrı modeli) doğrudan ilgili.
- **A Retargetable Hybrid Binary Translation System (HBT).** Shen ve ark., SIES 2012. ✅
  [PDF](https://people.cs.nycu.edu.tw/~wuuyang/homepage/papers/paper%2045-SIES20120610.pdf)
  Statik çeviri + bulunamayan dallanma hedefinde dinamik çeviriciye geçiş, adres
  tablosuyla geri dönüş. Bizim "dispatch'te bulunamayan adres" ve `genexec` yolunun
  2012'deki karşılığı.

### LLVM tabanlı lifter'lar

- **McSema: Static Translation of X86 Instructions to LLVM.** Dinaburg, Ruef. REcon 2014
  (sunum). 🧭 ✅ [slaytlar](https://www.recon.cx/2014/slides/McSema.pdf)
  Sonradan Remill komut-semantiği kütüphanesine ayrıldı. Akademik bildirisi yok ama
  aşağıdaki doğrulama makalelerinin çoğu McSema'yı hedef alıyor.
- **SecondWrite: A Compiler-level IR based Binary Analysis and Rewriting System.** Anand
  ve ark. EuroSys 2013. ✅ [PDF](https://terpconnect.umd.edu/~barua/anand-EUROSYS-2013.pdf)
  x86 → LLVM IR; yığın çerçevesini ve yerel değişkenleri geri kazanıp optimizasyonu
  mümkün kılıyor. Bizim "yığın bellekte kalıyor" sınırımızın aşılabileceğini gösteren
  erken iş.
- **rev.ng: A Unified Binary Analysis Framework to Recover CFGs and Function
  Boundaries.** Di Federico, Payer, Agosta. CC 2017. 🔒 (bu makineden erişilemedi)
  [yazar kopyası](https://www.nebelwelt.net/files/17CC.pdf)
  QEMU'nun ön ucuyla 17 ISA; noreturn, kuyruk çağrısı, bağlama duyarlı CFG analizleri.
  Fonksiyon sınırı kalitesi IDA/angr/BAP ile karşılaştırılabilir.
- **Raising Binaries to LLVM IR with MCTOLL.** Yadavalli, Smith (Microsoft). LCTES 2019
  (WIP). 🔒 [özet](https://conf.researchr.org/details/LCTES-2019/LCTES-2019-papers/15/Raising-Binaries-to-LLVM-IR-with-MCTOLL-Work-in-progress-)
  LLVM'in kendi MC katmanını kullanarak x64/Arm32 → LLVM IR. SIMD desteklemiyor.
- **Instrew: Leveraging LLVM for High Performance Dynamic Binary Instrumentation.**
  Engelke, Schulz. VEE 2020. ✅ [PDF](https://home.cit.tum.de/~engelke/pubs/2003-vee.pdf)
  Makine kodunu doğrudan LLVM IR'a lift edip JIT'liyor; Valgrind'in 1/5'i ek yük.
  Lift edilmiş koddan yüksek kalite çıkarmanın pratik dersleri var.

### Sezgisiz / tam kapsamlı çizgi (2020 → 2026)

- ⭐ **BinRec: Dynamic Binary Lifting and Recompilation.** Altinay, Nash ve ark.
  EuroSys 2020. ✅ [PDF](https://download.vusec.net/papers/binrec_eurosys20.pdf)
  Statik disassembly yerine **dinamik izlerden** LLVM IR'a lift; izlenmeyen yol çıkarsa
  geri dönüp öğreniyor. SPEC INT 2006'nın tamamı. Bizim kapsama aracımızın
  (`kapsama-araci`) "hiç çalışmayan fonksiyon" ölçümüyle birlikte düşünülmeli.
- ⭐ **Polynima: Practical Hybrid Recompilation for Multithreaded Binaries.** Deshpande,
  Parzefall, Hetzelt, Franz. EuroSys 2024. DOI 10.1145/3627703.3650065 (CC-BY, açık
  erişim; ACM sitesi bu makineden otomatik istemciyi reddetti). 🔒
  BinRec'in çok iş parçacıklı ikililere genişletilmesi.
- ⭐ **MirrorBall — No Bit Left Behind: Using Brute-Force Lifting to Achieve Fully
  Static Binary Recompilation.** Huang, Chen, Baron, Franz. arXiv 2609.16423, Eyl 2026. ✅
  [arXiv](https://arxiv.org/abs/2609.16423)
  Kod/veri ayırmayı reddediyor: **her bayt ofsetini olası dallanma hedefi sayıp**
  süper-küme CFG kuruyor; çözülemeyen dolaylı dallanmalar dispatch tablosuna düşüyor;
  orijinal ikili salt-okunur blob olarak gömülü (dizgeler, jump table, vtable geçerli
  kalıyor). Tam metinden: ikili boyutu ort. ~74×, aynı ISA'da ort. 3.74×, x86-64 →
  AArch64'te 6.64× yavaşlama; çok iş parçacığı, kendini değiştiren kod ve C++
  istisnaları yok; en büyük yükün **yazmaç durumunun iş parçacığı-yerel değişkenlerde
  tutulması (SSA'ya yükseltilemiyor)** olduğunu söylüyorlar.
  **Bize göre:** bizim yaklaşımımız tam tersi uçta (reloc tablosuyla kesin işaretçi
  bilgisi → %96 kod kapsamı, %1.29 belirsizlik, yerel hıza yakın). Kalan 47 KB
  belirsiz bölge için onların "her ofseti dene + dispatch" fikri sınırlı ölçekte
  uygulanabilir. Yazmaç durumu maliyeti gözlemi bizim MEMARG/`restrict` bulgumuzla
  aynı kökten.
- ⭐ **Elevator — Deterministic Fully-Static Whole-Binary Translation without
  Heuristics.** Chen, McGowan, Franz. arXiv 2605.08419, May 2026. ✅
  [arXiv](https://arxiv.org/abs/2605.08419)
  x86-64 → AArch64; her baytın her makul yorumu için ayrı çeviri, ISA tanımından
  otomatik türetilen "kod karoları". QEMU kullanıcı kipiyle eşit ya da daha hızlı;
  bedeli büyük kod şişmesi. Arama özetlerine göre (tam metni okumadım) makalede blok içi geriye bayrak canlılığı var ve **bayrak
  başına (EFLAGS bütünü yerine) canlılığı** gelecek iş olarak öneriyorlar — bizde
  bayraklar zaten ayrı alanlarda.
- **Formally Verified Lifting of C-compiled x86-64 Binaries.** Verbeek, Bockenek, Fu,
  Ravindran. PLDI 2022. ✅ [PDF](https://www.ssrg.ece.vt.edu/papers/pldi22.pdf)
  Dönüş adresi bütünlüğü ve çağrı kuralı uyumu gibi "akıl sağlığı" özelliklerini
  kanıtlayıp tüm yolları kapsayan (aşırı yaklaşık) lift; Isabelle/HOL'e aktarım.
  Xen'de ~400K komut.

### Bellek modeli ve çok iş parçacığı (x86 TSO → Arm)

Oyunda iş parçacıkları var (ses, yükleme). Apple Silicon'da Rosetta donanım TSO kipini
kullanıyor; bizim yerel ikilimiz kullanmıyor. Şu ana kadar bir yarış hatası
gözlenmedi ama literatür bu riskin ölçeğini gösteriyor:

- ⭐ **Lasagne: A Static Binary Translator for Weak Memory Model Architectures.** Rocha,
  Sprokholt ve ark. PLDI 2022. 🔒 (depo bu makineden erişilemedi)
  [St Andrews deposu](https://research-repository.st-andrews.ac.uk/handle/10023/25964)
  x86 → Arm statik çeviri + kanıtlanmış çit (fence) yerleştirme ve gereksiz çit
  eleme (ort. %45.5 daha az çit). Arancini makalesine göre Lasagne çevirileri yerelin
  yalnızca 2–3× gerisinde, ama jump table/işlev işaretçisi yoğun programları
  çeviremiyor.
- **Risotto: A Dynamic Binary Translator for Weak Memory Model Architectures.**
  Gouicem ve ark. ASPLOS 2023. DOI 10.1145/3567955.3567962. 🔒 (TU Delft deposu otomatik
  istemciyi reddetti) [TU Delft PDF](https://research.tudelft.nl/files/144252582/3567955.3567962.pdf)
  QEMU'nun x86-on-Arm çevirisinde bellek modeli hataları buluyor, kanıtlanmış eşleme
  şemaları öneriyor.
- ⭐ **Arancini: A Hybrid Binary Translator for Weak Memory Model Architectures.**
  Reimers, Sprokholt ve ark. ASPLOS 2026. ✅
  [PDF](https://www.st.ewi.tudelft.nl/sschakraborty/papers/ASPLOS2026-Arancini.pdf)
  Statik + hafif çalışma zamanı + istek üzerine dinamik çeviri; ortak IR (ArancinIR)
  bayrakları ve **80-bit x87 yazmaçlarını** doğrudan modelliyor. Tam metinden notlar:
  - Benzersiz komutların ezici çoğunluğu önceden çevriliyor; dinamik pay en fazla %2.1,
    çalışma zamanının asıl işi dinamik kontrol akışını statik koda eşlemek.
    **Bizim ölçümümüzle aynı tablo** (dolaylı çağrı tablosu + `genexec`).
  - **"Ölü bayrak eleme az kazandırdı, çünkü LLVM'in ölü kod elemesi zaten benzerini
    yapıyor."** Bu, bizim fonksiyon içi bayrak canlılığı ölçümümüzle
    (`bayrak-canlilik-kazanc-yok`: clang zaten eliyor) birebir örtüşüyor. Fonksiyonlar
    arası canlılık (bizde FLAGABI, −%6.9 komut) makalede yok.
  - Yerel kütüphanelere bağlanmak (bizdeki CRT/`rep` dizge işlemlerinin yerel
    karşılıklarla değiştirilmesi gibi) yükü 8.0× → 6.0× düşürüyor; `rep` komutlarını
    çevirmemek önemli kazanç.
  - Kendini değiştiren kodu açık sorun olarak bırakıyorlar.

---

## 2. Dinamik ikili çeviri (karşılaştırma ve ödünç alınabilecek teknikler)

- **QEMU, a Fast and Portable Dynamic Translator.** Bellard. USENIX ATC 2005 (FREENIX). ✅
  [PDF](https://www.usenix.org/legacy/event/usenix05/tech/freenix/full_papers/bellard/bellard.pdf)
  Tembel bayrak hesaplama (işlenenleri + işlem türünü sakla, istenince hesapla)
  burada ve QEMU belgelerinde anlatılıyor.
- **Dynamo: A Transparent Dynamic Optimization System.** Bala, Duesterwald, Banerjia.
  PLDI 2000. ✅ [PDF](https://www.cse.iitm.ac.in/~krishna/courses/2022/odd-cs6013/BalaDusterwaldBanerjiaPLDI2000.pdf)
  Sıcak iz seçimi ve çalışırken yeniden optimizasyon; bizim profil-güdümlü
  "sıcak fonksiyonu elle yerel C'ye taşı" (D3DX HLE Aşama 4) yaklaşımının otomatik hali.
- **The Transmeta Code Morphing Software.** Dehnert ve ark. CGO 2003. ✅
  [PDF](https://courses.cs.washington.edu/courses/cse548/08wi/papers/transmeta.pdf)
  Tam sistem x86 uyumluluğu yazılımla; spekülasyon, geri alma, uyarlamalı yeniden
  çeviri. x86'nın "gerçek hayat" köşe durumlarının (kendini değiştiren kod, kesin
  istisnalar) en iyi anlatımlarından.
- **HQEMU: A Multi-Threaded and Retargetable Dynamic Binary Translator on Multicores.**
  Hong ve ark. CGO 2012. 🔒 (yazar sayfası otomatik istemciyi reddetti)
  [PDF](https://cs.nthu.edu.tw/~ychung/conference/2012-CGO-HQEMU.pdf)
  QEMU + LLVM; arka plan iş parçacığında LLVM ile yeniden optimizasyon.
- **Virtualization Without Direct Execution or Jitting: Designing a Portable Virtual
  Machine Infrastructure.** Mihocka, Shwartsman. AMAS-BT 2008. ✅
  [PDF](https://bochs.sourceforge.io/Virtualization_Without_Hardware_Final.pdf)
  Bochs'un tembel bayrak gösterimi ve hızlı yorumlayıcı tasarımı. Bizim `genexec`
  hibrit yorumlayıcısı için doğrudan referans.
- ⭐ **ARMing x86 Games: Accelerating Binary Translation Using Software
  Optimizations.** Yen ve ark. MobiSys 2025. 🔒
  [özet](https://researchwith.stevens.edu/en/publications/arming-x86-games-accelerating-binary-translation-using-software-o/)
  **x86 oyunlarını Arm'da çalıştırma**; veri akışı analiziyle x86 bayraklarının ne
  zaman Arm'ın donanım bayraklarıyla (NZCV) karşılanabileceğini buluyor; Steam
  oyunlarında %7–12 FPS. Bizim lifter'ın C ürettiği için NZCV'yi doğrudan kullanamadığı
  düşünülürse, clang'ın `cmp`+`b.cc` örüntüsüne düşecek C kalıpları üretmek bu fikrin
  statik karşılığı olur.
- **Box64 / FEX-Emu.** 🧭 Box64'ün 2026 COSCUP sunumu
  ([özet](https://pretalx.coscup.org/coscup-2026/talk/PKYW3X/)): bayrak canlılığı,
  ertelenmiş bayrak hesabı, yerel bayrak yeniden kullanımı, "önce sarmala" (yerel
  kütüphanelere köprü). FEX blogu x87 için pratik optimizasyonlar anlatıyor
  ([FEX-2408](https://fex-emu.com/FEX-2408/): 80-bit kopyaları algılayıp bir blokta
  2340 → 165 komut).
- **Rosetta 2.** 🧭 Apple'ın yayımlanmış bir makalesi yok. Bağımsız ölçümler
  (Anandtech, 2020) yerelin ~%78–79'u diyor. Bizim karşılaştırmamız Wine ile
  (`WINE_KIYAS.md`), Rosetta+Wine ile değil.

---

## 3. Disassembly: kod keşfi, işaretçi/sabit ayrımı, fonksiyon sınırları, jump table

Projenin "en zor sorun" dediği (README: *telling a 4-byte address from a 4-byte number*)
konu. Bizim çözümümüz sıfırlanmış reloc dizinine rağmen fiziksel olarak duran reloc
verisini elle ayrıştırmak. Literatürde bunu yapan bir çalışma bulamadım (bkz. §12).

- ⭐ **An In-Depth Analysis of Disassembly on Full-Scale x86/x64 Binaries.** Andriesse,
  Chen, van der Veen, Slowinska, Bos. USENIX Security 2016. ✅
  [PDF](https://www.usenix.org/system/files/conference/usenixsecurity16/sec16_paper_andriesse.pdf)
  981 ikili, 9 disassembler. Satır içi veri ve örtüşen kod sanıldığından az sorun;
  **fonksiyon sınırları sanıldığından zor.** Bizim %96 kapsam / 23.274 fonksiyon
  sayılarını bağlama oturtmak için.
- ⭐ **SoK: All You Ever Wanted to Know About x86/x64 Binary Disassembly But Were Afraid
  to Ask.** Pang ve ark. IEEE S&P 2021. ✅ [arXiv](https://arxiv.org/abs/2007.14266)
  9 açık kaynak araç, 3.788 ikili. Tüm araçlar ilkeli algoritmalar kullansa da kapsam
  için ağır sezgilere dayanıyor; Ghidra/Dyninst jump table'ların %93'ünü ~%90
  doğrulukla çözüyor. Bizim 763 dolaylı atlamadan 446'sını jump table olarak
  çözmemizi bu oranlarla karşılaştırmak için önce kalan 317'nin kaçının gerçekten
  jump table olduğunu ayırmak gerekir.
- **Ramblr: Making Reassembly Great Again.** Wang ve ark. NDSS 2017. ✅
  [PDF](https://sites.cs.ucsb.edu/~vigna/publications/2017_NDSS_Ramblr.pdf)
  "Sembolleştirme" (hangi anlık değer işaretçi?) hatalarının sınıflandırması. **Reloc
  olmadan işaretçi/sabit ayrımının ne kadar zor olduğunun en iyi anlatımı.**
- **Superset Disassembly: Statically Rewriting x86 Binaries Without Heuristics
  (Multiverse).** Bauman, Lin, Hamlen. NDSS 2018. ✅
  [PDF](https://www.ndss-symposium.org/wp-content/uploads/2018/02/ndss2018_05A-4_Bauman_paper.pdf)
  Her ofsetten disassembly; MirrorBall'un atası.
- **Datalog Disassembly (Ddisasm).** Flores-Montoya, Schulte. USENIX Security 2020. ✅
  [PDF](https://www.usenix.org/system/files/sec20-flores-montoya.pdf)
  Souffle Datalog ile kod konumu, sembol, fonksiyon sınırı çıkarımı; **PE'yi ve x86-32'yi
  destekliyor** ve açık kaynak. Kendi keşif sonuçlarımızı bağımsız bir araçla
  karşılaştırmak için kullanılabilir.
- **ByteWeight: Learning to Recognize Functions in Binary Code.** Bao ve ark. USENIX
  Security 2014. ✅ [PDF](https://www.usenix.org/system/files/conference/usenixsecurity14/sec14-paper-bao.pdf)
- **Compiler-Agnostic Function Detection in Binaries (Nucleus).** Andriesse,
  Slowinska, Bos. EuroS&P 2017. ✅ [proje sayfası ve PDF](https://vusec.net/projects/function-detection)
- ⭐ **Padding Matters — Exploring Function Detection in PE Files.** Springer ve ark.
  CODASPY 2025. ✅ [arXiv](https://arxiv.org/abs/2504.21520)
  Literatürün ELF ağırlıklı olduğunu, **PE ikilileri için** fonksiyon tespitinin az
  çalışıldığını söylüyor; hizalama dolgusunun (int3/nop) araçları nasıl yanılttığını
  ölçüyor. Bizim "%98.71 açıklanan" hesabımızdaki hizalama/veri sınıflandırması için
  doğrudan ilgili.
- **Recovery of Jump Table Case Statements from Binary Code.** Cifuentes, Van Emmerik.
  *Sci. Comput. Program.* 40(2-3), 2001 (IWPC 1999). 🔒
  Dilimleme + ifade normal formu + derleyici kalıpları. Bizim jump table çözücümüzün
  klasik referansı.

---

## 4. Komut semantiği ve lifter doğrulama (Rosetta diferansiyel fuzzer'ının akrabaları)

Projenin en verimli hata bulma aracı: lift edilmiş komutu Rosetta 2 üzerinde çalışan
gerçek x86 koduna karşı çalıştırıp durumu karşılaştırmak (`rosetta-diferansiyel-fuzzer`).
Literatürdeki karşılıklar gerçek CPU'yu ya da başka bir emülatörü oracle olarak
kullanıyor; **Rosetta'yı oracle olarak kullanan bir çalışma bulamadım.**

- ⭐ **Testing CPU Emulators (EmuFuzzer).** Martignoni, Paleari, Fresi Roglia, Bruschi.
  ISSTA 2009; dergi hali TOSEM 2013. DOI 10.1145/1572272.1572303. 🔒
  Aynı test girdisini emülatörde ve fiziksel CPU'da çalıştırıp durumu karşılaştırma;
  QEMU, Valgrind, Pin, Bochs'ta hata. **Bizim fuzzer'ın birebir yöntemi.**
- **A Fistful of Red-Pills: How to Automatically Generate Procedures to Detect CPU
  Emulators.** Paleari, Martignoni, Fresi Roglia, Bruschi. WOOT 2009. ✅
  [PDF](https://static.usenix.org/events/woot09/tech/full_papers/paleari.pdf)
  Aynı grubun, emülatör ↔ gerçek CPU farklarını otomatik üreten önceki çalışması;
  yüzlerce opcode'da fark.
- ⭐ **Path-Exploration Lifting: Hi-Fi Tests for Lo-Fi Emulators.** Martignoni,
  McCamant, Poosankam, Song, Maniatis. ASPLOS 2012. ✅
  [PDF](https://people.eecs.berkeley.edu/~dawnsong/papers/2012%20Path%20Exploration%20Lifting%20Hi%20Fi%20Tests%20for%20Lo%20Fi%20Emulators.pdf)
  Yüksek doğruluklu emülatörü sembolik yürütmeyle gezip **komut başına yol
  kapsamı tam** test girdileri üretiyor (~610 bin test, komutların ~%95'inde tam yol
  kapsamı). Bizim rastgele fuzzer'ımıza "her komutun her dalını kapsayan girdi"
  eklemek için doğrudan uygulanabilir.
- **N-Version Disassembly: Differential Testing of x86 Disassemblers.** Paleari ve ark.
  ISSTA 2010. DOI 10.1145/1831708.1831741. 🔒 CPU'yu referans kod çözücü olarak
  kullanıyor; sekiz disassembler'ın hepsinde hata.
- **Differential Analysis of x86-64 Instruction Decoders (Mishegos).** Woodruff, Carroll,
  Peters. 2021. ✅ [EasyChair preprint](https://easychair.org/publications/preprint/1LHr/open)
  Capstone dahil kod çözücülerde farklar. **Bizim lifter Capstone'a dayanıyor;**
  `capstone-x87-eflags-tuzagi` türü hataların genel hali.
- ⭐ **Testing Intermediate Representations for Binary Analysis (MeanDiff).** Kim ve ark.
  ASE 2017. ✅ [PDF](https://softsec.kaist.ac.kr/~soomink/paper/ase17main-mainp491-p.pdf)
  Lifter'ların ürettiği IR'ları birbirine karşı karşılaştırıp 3 lifter'da 24 anlamsal
  hata.
- **Stratified Synthesis: Automatically Learning the x86-64 Instruction Set.** Heule,
  Schkufza, Sharma, Aiken. PLDI 2016. 🔒
  [özet](https://pldi16.sigplan.org/details/pldi-2016-papers/50/Stratified-Synthesis-Automatically-Learning-the-x86-64-Instruction-Set)
  Komut semantiğini donanımdan sentezleyerek öğreniyor; elle yazılmış semantikte
  onlarca hata.
- **A Complete Formal Semantics of x86-64 User-Level Instruction Set Architecture.**
  Dasgupta, Park, Kasampalis, Adve, Roşu. PLDI 2019. ✅
  [PDF](https://fsl.cs.illinois.edu/publications/dasgupta-park-kasampalis-adve-rosu-2019-pldi.pdf)
  K çerçevesinde 3155 komut varyantı; çalıştırılabilir. x87 kapsam dışı ama tamsayı /
  bayrak semantiği için referans.
- **Scalable Validation of Binary Lifters.** Dasgupta, Dinesh, Venkatesh, Adve,
  Fletcher. PLDI 2020. DOI 10.1145/3385412.3385964. 🔒 (aynı başlıklı doktora tezi
  [IDEALS](https://www.ideals.illinois.edu/items/115576), bu makineden otomatik
  istemciye kapalı)
  Tek komut düzeyinde biçimsel çeviri doğrulama; McSema'da 29 yeni hata.
- **ISA Semantics for ARMv8-A, RISC-V, and CHERI-MIPS (Sail).** Armstrong ve ark. POPL
  2019. ✅ [PDF](https://www.cl.cam.ac.uk/~pes20/sail/sail-popl2019.pdf)
  Hedef tarafın (ARM64) resmi semantiği; doğrudan ihtiyacımız yok, ama x86 için
  benzer bir kaynak (K semantiği) ile birlikte "çeviri doğrulama" zincirinin iki ucu.

---

## 5. Bayraklar (EFLAGS) ve x87

### Bayraklar

- **Register Liveness Analysis for Optimizing Dynamic Binary Translation.** Probst,
  Krall, Scholz. WCRE 2002. 🔒
  [yazar sayfası](https://www.complang.tuwien.ac.at/andi/papers/wcre_02.html)
  Hızlı canlılık analizi; gereksiz saklamalarda ~%50 azalma, %10–30 hızlanma.
- **Leveraging On-Chip Debug Features for Condition Codes Handling in Dynamic Binary
  Translation.** Salgado ve ark. SBESC 2014. ✅ [PDF](https://sbesc.lisha.ufsc.br/sbesc2014/dl202)
  Tembel bayrak tekniklerinin kısa sınıflandırması (tembel değerlendirme, ISA
  uzantısı, donanım desteği) + "süper tembel" yöntem.
- Arancini (§1), ARMing x86 Games (§2), Bochs (§2), QEMU (§2): bayrak tartışmasının
  geri kalanı burada. **Sonuç:** derleyici tabanlı (LLVM/clang) çeviricilerde fonksiyon
  içi ölü bayrak eleme derleyiciye bırakılabilir; kazanç fonksiyonlar arası
  canlılıktan ve donanım bayraklarının yeniden kullanımından geliyor.

### x87

- ⭐ **The Pitfalls of Verifying Floating-Point Computations.** Monniaux. ACM TOPLAS 30(3),
  2008. ✅ [arXiv](https://arxiv.org/abs/cs/0701192)
  x87'nin 80-bit iç hassasiyeti, belleğe yazarken çift yuvarlama, derleyici
  seçimlerine bağlı sonuçlar. Bizim "x87 64-bit double ile modelleniyor" sınırımızın
  ne zaman görünür fark üreteceğini anlamak için temel metin.
- **Correct Rounding in Double Extended Precision.** LORIA, 2025 (HAL hal-04861251). ✅
  [HAL](https://hal-emse.ccsd.cnrs.fr/LORIA-ALGO/hal-04861251v1)
  80-bit biçim için double-double aritmetiğiyle doğru yuvarlanan rutinler; x87'ye
  ihtiyaç duymadan. Tam 80-bit gerekirse (ör. bir fiziğin ıraksadığı bulunursa)
  uygulanabilir yol.
- Literatürde **x87'yi statik çeviride ele alan akademik bir çalışma bulamadım.**
  Pratik kaynaklar: FEX blogu (yukarıda), felix86 (64-bit double ile, "çoğu oyun için
  yeterli"), LoongArch'ın LBT donanım uzantısı (x87 TOP'u donanımda tutuyor).
  Bizim `fpush-degerlendirme-sirasi`, `x87-yuvarlama-hatasi` ve X87OPT (blok içi x87
  yazmaç tahsisi, −%10.9 komut) bulguları bu boşlukta yayımlanabilir nitelikte.

---

## 6. Üretilen C'nin derlenmesi: tanımsız davranış, takma ad, sandbox belleği

Bizim lifter C ürettiği için C'nin kendi tuzakları literatürle doğrudan kesişiyor.

- ⭐ **Towards Optimization-Safe Systems: Analyzing the Impact of Undefined Behavior
  (STACK).** Wang, Zeldovich, Kaashoek, Solar-Lezama. SOSP 2013. ✅
  [PDF](https://pdos.csail.mit.edu/papers/stack%3Asosp13.pdf)
  Derleyicinin UB yüzünden kod sildiği "optimizasyona kararsız" kod. Bizim
  `strict-aliasing-ub` (GVN'in lift kodunu yanlış derlemesi, opt5 farkının kökü)
  hatasının genel sınıfı.
- **Exploring C Semantics and Pointer Provenance (Cerberus).** Memarian ve ark. POPL
  2019. ✅ [PDF](https://www.cl.cam.ac.uk/~pes20/cerberus/cerberus-popl2019.pdf)
  İşaretçi kökeni (provenance) ve tamsayı ↔ işaretçi dönüşümleri. Misafir belleğini
  `uint8_t*` + ofset olarak gören kodumuzun hangi varsayımlara dayandığını netleştirir.
- **Not So Fast: Analyzing the Performance of WebAssembly vs. Native Code.** Jangda,
  Powers, Berger, Guha. USENIX ATC 2019. ✅ [arXiv](https://arxiv.org/abs/1901.09056)
  Wasm (taban + ofset bellek, bizim misafir belleğine çok benzer model) yerelden
  %45–55 yavaş; yazmaç baskısı ve ek adres hesabı kaynakları. Bizim lift kodunun
  yerel koda göre nerede kaybettiğini analiz etmek için iyi bir şablon.
- **Segue & ColorGuard: Optimizing SFI Performance and Scalability on Modern
  Architectures.** Narayan ve ark. ASPLOS 2025. ✅
  [PDF](https://cseweb.ucsd.edu/~dstefan/pubs/narayan:2025:segue.pdf)
  Taban adres toplamasının maliyetini x86 segment yazmacıyla kaldırıyor (%11–14).
  ARM64'te bizim `ldr w8, [x_base, w_addr, uxtw]` zaten bedava; ama **taban işaretçisini
  her yazmadan sonra yeniden yükleme** maliyeti (bizim MEMARG ile çözdüğümüz) aynı
  ailenin sorunu.
- **Retrofitting Fine Grain Isolation in the Firefox Renderer (RLBox).** Narayan ve ark.
  USENIX Security 2020. ✅ [arXiv](https://arxiv.org/abs/2003.00572)
  wasm2c ile Wasm → C → yerel; "C'yi hedef dil olarak kullanan çeviri" üretimde.

---

## 7. İşletim sistemi / API katmanı ve yüksek düzey emülasyon (HLE)

- **Rethinking the Library OS from the Top Down (Drawbridge).** Porter, Boyd-Wickizer,
  Howell, Olinsky, Hunt. ASPLOS 2011. ✅ [PDF](https://www.cs.unc.edu/~porter/pubs/p291-porter.pdf)
  Windows 7'yi küçük bir çekirdek arayüzü (iş parçacığı, sanal bellek, G/Ç akışı)
  üzerinde kütüphane OS olarak çalıştırıyor; DirectX dahil. **Bizim el yazımı Win32
  katmanımızın hangi soyutlamalara indirgenebileceğinin akademik referansı.**
- ⭐ **HALucinator: Firmware Re-hosting Through Abstraction Layer Emulation.** Clements
  ve ark. USENIX Security 2020. ✅ [PDF](https://www.usenix.org/system/files/sec20-clements.pdf)
  İkilideki HAL kütüphane fonksiyonlarını eşleştirip yerine genel uygulamalar koyuyor;
  makale bunu açıkça "yüksek düzey emülasyon (HLE)" diye adlandırıyor. **Bizim D3DX9
  HLE çalışmasının (`D3DX_HLE.md`) doğrudan karşılığı;** kütüphane eşleştirme kısmı,
  statik bağlı D3DX/CRT fonksiyonlarını otomatik tanımak için uygulanabilir.
- **Statik bağlı kütüphane tanıma (FLIRT alternatifleri).**
  - Arutunian, Aslanyan. *Proceedings of ISP RAS* 38(4), 2026 — program bağımlılık
    grafiği + çağrı grafiğiyle gömülü kütüphane tanıma (ort. %91 kesinlik, %99 duyarlılık). ✅
    [PDF](https://ispras.ru/proceedings/docs/2026/38/4-1/isp_38_2026_4_89.pdf)
  - Carrillo-Mondéjar, Rodríguez. *MANTILLA*, Future Generation Computer Systems 2025,
    DOI 10.1016/j.future.2024.107602. 🔒
  Oyun MSVC CRT ve D3DX9'u statik bağlamış; bunları tanıyıp yerel karşılıklarla
  değiştirmek hem hız (Arancini'deki "yerel kütüphane" kazancı) hem doğruluk sağlar.
- **Wine.** 🧭 Hakemli bir mimari/performans makalesi bulamadım. Bizim
  `WINE_KIYAS.md` ölçümü (yarışta yerel 119 fps, Wine 10–15 fps) bu boşlukta tek veri.

---

## 8. Grafik API ve shader çevirisi (D3D9 → Metal, vs/ps 1.x–2.x → MSL)

**D3D9 → Metal ya da DXBC → MSL çevirisini ele alan akademik bir makale bulamadım.**
Alan tamamen endüstri ve açık kaynak araçlarıyla (DXVK, wined3d, MoltenVK,
SPIRV-Cross, dxil-spirv) ilerlemiş. Doğrulama tarafında ise güçlü akademik iş var:

- ⭐ **Automated Testing of Graphics Shader Compilers (GLFuzz → GraphicsFuzz).**
  Donaldson, Evrard, Lascu, Thomson. OOPSLA 2017. ✅
  [PDF](https://www.doc.ic.ac.uk/~afd/papers/2017/OOPSLA.pdf)
  Anlamı koruyan dönüşümlerle shader çeşitleri üretip görüntü farkından derleyici
  hatası bulma; 17 GPU/sürücüde 60+ hata. **Bizim vs/ps → MSL çevirimize uygulanabilir:**
  oyunun kendi shader'larını anlamı koruyan biçimde dönüştürüp çeviri sonrası çıktıyı
  karşılaştırmak, `msl-yazma-maskesi`, `expp-deri-eklem`, `d3d9-renk-kirpma` türü
  hataları sistematik bulurdu.
- **Test-Case Reduction and Deduplication Almost for Free with Transformation-Based
  Compiler Testing (spirv-fuzz).** Donaldson ve ark. PLDI 2021. ✅
  [PDF](https://www.doc.ic.ac.uk/~afd/homepages/papers/pdfs/2021/PLDI.pdf)
  Hata tetikleyen dönüşüm dizisini otomatik küçültme.
- 🧭 **MoltenVK: Application Portability with Vulkan on Metal.** Vulkanised 2023. ✅
  [slaytlar](https://dev.vulkan.org/user/pages/09.events/vulkanised-2023/vulkanised_2023_moltenvk_application_portability_with_vulkan_on_metal.pdf)
  Vulkan → Metal katmanının kaçınılmaz bellek/performans bedeli; "DirectX kökenli ikili
  portlar" performans sınırına takılan başlıklar arasında sayılıyor. Bizim doğrudan
  D3D9 → Metal tercihimizin (ara katman yok) gerekçesi.
- 🧭 **My personal hell of translating DXIL to SPIR-V.** Themaister (vkd3d-proton), 2021. ✅
  [blog](https://themaister.net/blog/2021/09/05/my-personal-hell-of-translating-dxil-to-spir-v-part-1/)
  Shader ara dillerini çevirmenin derleyici teorisi tarafı; yapısız kontrol akışı vb.
- **Birincil spesifikasyonlar** (makale değil ama doğruluk kaynağı): Microsoft
  "Directly Mapping Texels to Pixels (Direct3D 9)" (yarım piksel kuralı — bizim
  `d3d9-yarim-piksel`), D3D9 shader model 1.x–3.0 başvuru belgeleri, Apple Metal Shading
  Language Specification.

---

## 9. Grafik iyileştirmeleri (HD build: SSR, ışın izlemeli yansıma, filtreleme)

- **Efficient GPU Screen-Space Ray Tracing.** McGuire, Mara. *JCGT* 3(4), 2014. ✅
  [PDF](https://jcgt.org/published/0003/04/04/paper.pdf)
  Perspektif-doğru DDA ile derinlik tamponunda ışın yürütme; SSR'ın standart referansı.
- **An Adaptive Acceleration Structure for Screen-Space Ray Tracing.** Widmer ve ark.
  HPG 2015. ✅ [PDF](https://research.nvidia.com/sites/default/files/pubs/2015-08_An-Adaptive-Acceleration/AcceleratedSSRT_HPG15.pdf)
  Hiyerarşik derinlikle hızlandırılmış SSR.
- **Hybrid Rendering for Real-Time Ray Tracing (PICA PICA).** Barré-Brisebois ve ark.
  *Ray Tracing Gems*, bölüm 25, 2019 (kitap açık erişim). ✅
  [kitap](https://link.springer.com/book/10.1007/978-1-4842-4427-2) ·
  [EA SEED](https://ea.com/seed/news/hybrid-rendering-for-real-time-ray-tracing)
  Raster + ışın izleme karma boru hattı, parlak yansımalar. Bizim yarım çözünürlüklü
  RT yansıma + G-tampon ayrıştırma tasarımına en yakın kaynak. *Ray Tracing Gems II*
  (2021) de açık erişim.
- **Spatiotemporal Variance-Guided Filtering (SVGF).** Schied ve ark. HPG 2017. ✅
  [PDF](https://cg.ivd.kit.edu/publications/2017/svgf/svgf_preprint.pdf)
  Piksel başına 1 örnekten zamansal birikim + varyans güdümlü filtre. Yarım
  çözünürlüklü yansımayı gürültüsüz büyütmek için aday.
- `rt-zehirli-ivme-yapisi` (NaN/uzak köşe ivme yapısını zehirliyor) için doğrudan bir
  makale bulamadım; *Ray Tracing Gems* bölüm 6 ("A Fast and Robust Method for Avoiding
  Self-Intersection") ve BVH sağlamlığı üzerine bölümler en yakın kaynaklar.

---

## 10. Ölçüm yöntemi, performans ve enerji

- ⭐ **Producing Wrong Data Without Doing Anything Obviously Wrong!** Mytkowicz, Diwan,
  Hauswirth, Sweeney. ASPLOS 2009. ✅ [proje sayfası](https://sape.inf.usi.ch/publications/asplos09)
  Ortam değişkeni boyutu, bağlama sırası gibi masum değişikliklerin ölçümü yanlı
  yapması. Bizim `perf-olcum-yontemi` (tek koşu %7 gürültülü),
  `kare-freni-olcumu-maskeliyordu` ve `dumpframe-olcum-artefakti` bulgularının
  kuramsal çerçevesi; 128 shard'ın bağlama sırası bile sonucu oynatabilir.
- **Rigorous Benchmarking in Reasonable Time.** Kalibera, Jones. ISMM 2013. ✅
  [Kent deposu](https://kar.kent.ac.uk/33611/)
  Hangi düzeyde (derleme / koşu / yineleme) kaç tekrar gerektiğini hesaplayan model.
  A/B ölçümlerimizde (ABTEST, dönüşümlü koşular) tekrar sayısını gerekçelendirmek için.
- **Apple vs. Oranges: Evaluating the Apple Silicon M-Series SoCs for HPC Performance and
  Efficiency.** Hübner ve ark. arXiv 2502.05317, 2025. ✅ [arXiv](https://arxiv.org/abs/2502.05317)
  M1–M4'te `powermetrics` ile güç ölçümü. Not: `powermetrics` modele dayalı tahmin,
  duvar ölçümü değil; göreli karşılaştırmalar daha güvenilir. Bizim `enerji-olcumu`
  ve `gpu-enerji-ab` yöntemleriyle uyumlu.
- **RAVEN: Perception-aware Optimization of Power Consumption for Mobile Games.** MobiCom
  2017. 🔒 Algısal olarak aynı kareleri atlayarak ort. %21.8 enerji tasarrufu.
- **Yan ve ark., HotPower 2015.** ✅
  [PDF](https://engineering.purdue.edu/~hotpower/HotPower_2015_paper_6.pdf)
  40 mobil oyun; sabit yüksek kare hızının durağan sahnede israf olduğu, dinamik kare
  hızı ölçekleme. Bizim `hd-surus-gpu-tavani` ("duran araba GPU 8 W") gözlemiyle aynı
  fikir.

---

## 11. Dil modeli tabanlı çeviri (yeni, deneysel)

- **From CISC to RISC: Language-Model Guided Assembly Transpilation (CRT).** Heakl ve
  ark. arXiv 2411.16341, 2024. ✅ [arXiv](https://arxiv.org/abs/2411.16341)
  x86 → ARM assembly; ~%79 doğruluk. Tüm-program doğruluğu gereken bir oyun için
  uygun değil, ama tek tek "trap" kalan nadir komutların (bizde 400 tane) taslak
  çevirisi için düşünülebilir.
- **Guaranteed Guess: A Language Modeling Approach for CISC-to-RISC Transpilation with
  Testing Guarantees.** arXiv 2506.14606, 2025. ✅ [arXiv](https://arxiv.org/abs/2506.14606)
  Dil modeli çıktısını testle güvenceye alma; bizim fuzzer'ımız böyle bir güvence
  katmanı olarak zaten var.

---

## 12. Oyun koruma ve hukuki çerçeve

- **Making videogame history: videogame preservation and copyright law.** Y. Lee.
  *Interactive Entertainment Law Review* 1(2), 2018. ✅ [White Rose deposu](https://eprints.whiterose.ac.uk/137835/)
  Tersine mühendislik, decompilation ve koruma istisnaları. README'deki "oyun kodu ve
  varlıkları dağıtılmıyor" kararının hukuki arka planı.
- Statik rekompilasyonla oyun portlamayı (N64Recomp, XenonRecomp, xboxrecomp, M-HT/SR)
  ele alan **hakemli bir makale bulamadım**; alan tamamen topluluk projeleriyle ilerliyor.

---

## 13. Literatürde bulamadıklarım (projenin olası katkıları)

Tarama sonunda, projede yapılmış ama literatürde karşılığını bulamadığım işler:

1. **Sıfırlanmış reloc dizinine rağmen fiziksel olarak duran reloc verisini kurtarıp
   işaretçi/sabit ayrımında kullanmak.** Literatür ya reloc'un *var* olduğunu varsayıyor
   (Ramblr/Ddisasm bunu yardımcı bilgi sayar) ya da hiç olmadığını (Multiverse,
   MirrorBall). "Silinmiş görünen ama duran" durum ele alınmamış.
2. **Rosetta 2'yi oracle olarak kullanan diferansiyel komut fuzzer'ı.** EmuFuzzer ve
   Mishegos fiziksel x86 CPU kullanıyor; Apple Silicon üzerinde x86 donanımı olmadan
   aynı yöntemi Rosetta ile kurmak yeni.
3. **x87'nin statik çeviride ele alınması** (yığın değerlendirme sırası, RC alanına uyan
   `fistp`, blok içi x87 yazmaç tahsisi). Akademik iş yok; FEX/felix86 blogları dışında
   kaynak yok.
4. **23 bin fonksiyonluk bir Win32 + D3D9 3B oyununun uçtan uca statik rekompilasyonu**
   ve Wine'a karşı ölçülmüş performans/enerji. Lasagne/Arancini/MirrorBall SPEC ve
   Phoenix gibi kıyas setleri kullanıyor; gerçek bir etkileşimli uygulama yok.
5. **D3D9 shader bytecode → MSL çevirisinin doğrulanması** (görüntü farkı + Windows
   referansı). GraphicsFuzz yöntemi var ama API/bytecode çevirisine uygulanmamış.

Bu beşi bir atölye bildirisi (ör. AMAS-BT/VEE/CGO atölyeleri, ya da "Binary Analysis
Research" NDSS atölyesi) için yeterli malzeme gibi görünüyor.

---

## 14. Literatürden çıkan somut denemeler (öneri, önem sırasıyla)

1. **Kalan belirsiz baytlar için MirrorBall tarzı süper-küme lift** (yalnız 47 KB /
   1.546 blokta): her ofsetten deneme + dispatch tablosu. `genexec` yolunu daha da
   seyrekleştirir.
2. **Fuzzer'a yol kapsamı güdümlü girdi üretimi** (Hi-Fi/Lo-Fi): rastgele yerine her
   komutun her dalını kapsayan değerler (taşma sınırları, sıfıra bölme, x87 özel
   değerleri). Özellikle 400 "trap" komutu açılırken.
3. **Shader çevirisi için metamorfik test** (GraphicsFuzz): oyunun shader'larına anlamı
   koruyan dönüşümler uygulayıp Metal çıktısını karşılaştırma.
4. **Statik bağlı CRT/D3DX fonksiyonlarını otomatik tanıma** (HALucinator'ın eşleştirme
   adımı / FLIRT tarzı imzalar) ve yerel karşılıklarla değiştirme; Arancini'deki
   "yerel kütüphane" kazancının bizdeki karşılığı.
5. **Donanım bayrağı yeniden kullanımı** (ARMing x86 Games): `cmp`/`jcc` çiftlerini
   clang'ın tek `cmp` + `b.cc`'ye indireceği C kalıplarıyla üretmek.
6. **Ölçümde bağlama sırası rastgeleleştirme** (Mytkowicz): shard bağlama sırasını
   değiştirerek bir optimizasyonun kazancının yerleşim etkisinden ayrılması.
7. **Çok iş parçacıklı bölümlerde bellek modeli denetimi** (Lasagne/Risotto): ses ve
   yükleme iş parçacıklarının paylaştığı veriler için TSO varsayımı gerekip gerekmediği.

---

## Erişim notu

🔒 işaretli kaynakların çoğu ACM/IEEE'de; bunlar için yazar sayfası ya da kurum deposu
bulunamadı veya bu makineden erişilemedi (bazı depolar otomatik istemcileri
reddediyor; tarayıcıdan açılabilir). Hiçbir PDF depoya indirilmedi.
