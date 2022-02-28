#include "../main.h"
#include "../core/global.h"

#include "../search/asyncbot.h"
#include "../core/config_parser.h"
#include "../program/setup.h"
#include "../program/playutils.h"
#include "../core/timer.h"
using namespace std;

static bool noWhiteStonesOnBoard(const Board& board) {
  for(int y = 0; y < board.y_size; y++) {
    for(int x = 0; x < board.x_size; x++) {
      Loc loc = Location::getLoc(x,y,board.x_size);
      if(board.colors[loc] == P_WHITE)
        return false;
    }
  }
  return true;
}

static double getBoardSizeScaling(const Board& board) {
  return pow(19.0 * 19.0 / (double)(board.x_size * board.y_size), 0.75);
}

static double getPointsThresholdForHandicapGame(double boardSizeScaling) {
  return std::max(4.0 / boardSizeScaling, 2.0);
}

static double initialBlackAdvantage(const BoardHistory& hist) {
  BoardHistory histCopy = hist;
  histCopy.setAssumeMultipleStartingBlackMovesAreHandicap(true);
  int handicapStones = histCopy.computeNumHandicapStones();
  if(handicapStones <= 1)
    return 7.0 - hist.rules.komi;

  //Subtract one since white gets the first move afterward
  int extraBlackStones = handicapStones - 1;
  double stoneValue = hist.rules.scoringRule == Rules::SCORING_AREA ? 15.0 : 14.0;
  double whiteHandicapBonus = 0.0;
  if(hist.rules.whiteHandicapBonusRule == Rules::WHB_N)
    whiteHandicapBonus += handicapStones;
  else if(hist.rules.whiteHandicapBonusRule == Rules::WHB_N_MINUS_ONE)
    whiteHandicapBonus += handicapStones-1;

  return stoneValue * extraBlackStones + (7.0 - hist.rules.komi - whiteHandicapBonus);
}
static bool shouldResign(
  const Board& board,
  const BoardHistory& hist,
  Player pla,
  const vector<double>& recentWinLossValues,
  double lead,
  const double resignThreshold,
  const int resignConsecTurns,
  const double resignMinScoreDifference
) {
  double initialBlackAdvantageInPoints = initialBlackAdvantage(hist);

  int minTurnForResignation = 0;
  double noResignationWhenWhiteScoreAbove = board.x_size * board.y_size;
  if(initialBlackAdvantageInPoints > 0.9 && pla == P_WHITE) {
    //Play at least some moves no matter what
    minTurnForResignation = 1 + board.x_size * board.y_size / 5;

    //In a handicap game, also only resign if the lead difference is well behind schedule assuming
    //that we're supposed to catch up over many moves.
    double numTurnsToCatchUp = 0.60 * board.x_size * board.y_size - minTurnForResignation;
    double numTurnsSpent = (double)(hist.moveHistory.size()) - minTurnForResignation;
    if(numTurnsToCatchUp <= 1.0)
      numTurnsToCatchUp = 1.0;
    if(numTurnsSpent <= 0.0)
      numTurnsSpent = 0.0;
    if(numTurnsSpent > numTurnsToCatchUp)
      numTurnsSpent = numTurnsToCatchUp;

    double resignScore = -initialBlackAdvantageInPoints * ((numTurnsToCatchUp - numTurnsSpent) / numTurnsToCatchUp);
    resignScore -= 5.0; //Always require at least a 5 point buffer
    resignScore -= initialBlackAdvantageInPoints * 0.15; //And also require a 15% of the initial handicap

    noResignationWhenWhiteScoreAbove = resignScore;
  }

  if(hist.moveHistory.size() < minTurnForResignation)
    return false;
  if(pla == P_WHITE && lead > noResignationWhenWhiteScoreAbove)
    return false;
  if(resignConsecTurns > recentWinLossValues.size())
    return false;
  //Don't resign close games.
  if((pla == P_WHITE && lead > -resignMinScoreDifference) || (pla == P_BLACK && lead < resignMinScoreDifference))
    return false;

  for(int i = 0; i<resignConsecTurns; i++) {
    double winLossValue = recentWinLossValues[recentWinLossValues.size()-1-i];
    Player resignPlayerThisTurn = C_EMPTY;
    if(winLossValue < resignThreshold)
      resignPlayerThisTurn = P_WHITE;
    else if(winLossValue > -resignThreshold)
      resignPlayerThisTurn = P_BLACK;

    if(resignPlayerThisTurn != pla)
      return false;
  }

  return true;
}
static void updateDynamicPDAHelper(
  const Board& board, const BoardHistory& hist,
  const double dynamicPlayoutDoublingAdvantageCapPerOppLead,
  const vector<double>& recentWinLossValues,
  double& desiredDynamicPDAForWhite
) {
  (void)board;
  if(dynamicPlayoutDoublingAdvantageCapPerOppLead <= 0.0) {
    desiredDynamicPDAForWhite = 0.0;
  }
  else {
    double boardSizeScaling = getBoardSizeScaling(board);
    double pdaScalingStartPoints = getPointsThresholdForHandicapGame(boardSizeScaling);
    double initialBlackAdvantageInPoints = initialBlackAdvantage(hist);
    Player disadvantagedPla = initialBlackAdvantageInPoints >= 0 ? P_WHITE : P_BLACK;
    double initialAdvantageInPoints = abs(initialBlackAdvantageInPoints);
    if(initialAdvantageInPoints < pdaScalingStartPoints || board.x_size <= 7 || board.y_size <= 7) {
      desiredDynamicPDAForWhite = 0.0;
    }
    else {
      double desiredDynamicPDAForDisadvantagedPla =
        (disadvantagedPla == P_WHITE) ? desiredDynamicPDAForWhite : -desiredDynamicPDAForWhite;

      //What increment to adjust desiredPDA at.
      //Power of 2 to avoid any rounding issues.
      const double increment = 0.125;

      //Hard cap of 2.75 in this parameter, since more extreme values start to reach into values without good training.
      //Scale mildly with board size - small board a given point lead counts as "more".
      double pdaCap = std::min(
        2.75,
        dynamicPlayoutDoublingAdvantageCapPerOppLead *
        (initialAdvantageInPoints - pdaScalingStartPoints) * boardSizeScaling
      );
      pdaCap = round(pdaCap / increment) * increment;

      //No history, or literally no white stones on board? Then this is a new game or a newly set position
      if(recentWinLossValues.size() <= 0 || noWhiteStonesOnBoard(board)) {
        //Just use the cap
        desiredDynamicPDAForDisadvantagedPla = pdaCap;
      }
      else {
        double winLossValue = recentWinLossValues[recentWinLossValues.size()-1];
        //Convert to perspective of disadvantagedPla
        if(disadvantagedPla == P_BLACK)
          winLossValue = -winLossValue;

        //Keep winLossValue between 5% and 25%, subject to available caps.
        if(winLossValue < -0.9)
          desiredDynamicPDAForDisadvantagedPla = desiredDynamicPDAForDisadvantagedPla + 0.125;
        else if(winLossValue > -0.5)
          desiredDynamicPDAForDisadvantagedPla = desiredDynamicPDAForDisadvantagedPla - 0.125;

        desiredDynamicPDAForDisadvantagedPla = std::max(desiredDynamicPDAForDisadvantagedPla, 0.0);
        desiredDynamicPDAForDisadvantagedPla = std::min(desiredDynamicPDAForDisadvantagedPla, pdaCap);
      }

      desiredDynamicPDAForWhite = (disadvantagedPla == P_WHITE) ? desiredDynamicPDAForDisadvantagedPla : -desiredDynamicPDAForDisadvantagedPla;
    }
  }
}


struct GTPEngine {
  GTPEngine(const GTPEngine&) = delete;
  GTPEngine& operator=(const GTPEngine&) = delete;

  const string nnModelFile;
  const bool assumeMultipleStartingBlackMovesAreHandicap;
  const int analysisPVLen;
  const bool preventEncore;

  const double dynamicPlayoutDoublingAdvantageCapPerOppLead;
  double staticPlayoutDoublingAdvantage;
  bool staticPDATakesPrecedence;
  double normalAvoidRepeatedPatternUtility;
  double handicapAvoidRepeatedPatternUtility;

  double genmoveWideRootNoise;
  double analysisWideRootNoise;
  bool genmoveAntiMirror;
  bool analysisAntiMirror;

  NNEvaluator* nnEval;
  AsyncBot* bot;
  Rules currentRules; //Should always be the same as the rules in bot, if bot is not NULL.

  //Stores the params we want to be using during genmoves or analysis
  SearchParams params;

  TimeControls bTimeControls;
  TimeControls wTimeControls;

  //This move history doesn't get cleared upon consecutive moves by the same side, and is used
  //for undo, whereas the one in search does.
  Board initialBoard;
  Player initialPla;
  vector<Move> moveHistory;

  vector<double> recentWinLossValues;
  double lastSearchFactor;
  double desiredDynamicPDAForWhite;
  bool avoidMYTDaggerHack;
  std::unique_ptr<PatternBonusTable> patternBonusTable;

  Player perspective;

  double genmoveTimeSum;

  GTPEngine(
    const string& modelFile, SearchParams initialParams, Rules initialRules,
    bool assumeMultiBlackHandicap, bool prevtEncore,
    double dynamicPDACapPerOppLead, double staticPDA, bool staticPDAPrecedence,
    double normAvoidRepeatedPatternUtility, double hcapAvoidRepeatedPatternUtility,
    bool avoidDagger,
    double genmoveWRN, double analysisWRN,
    bool genmoveAntiMir, bool analysisAntiMir,
    Player persp, int pvLen,
    std::unique_ptr<PatternBonusTable>&& pbTable
  )
    :nnModelFile(modelFile),
     assumeMultipleStartingBlackMovesAreHandicap(assumeMultiBlackHandicap),
     analysisPVLen(pvLen),
     preventEncore(prevtEncore),
     dynamicPlayoutDoublingAdvantageCapPerOppLead(dynamicPDACapPerOppLead),
     staticPlayoutDoublingAdvantage(staticPDA),
     staticPDATakesPrecedence(staticPDAPrecedence),
     normalAvoidRepeatedPatternUtility(normAvoidRepeatedPatternUtility),
     handicapAvoidRepeatedPatternUtility(hcapAvoidRepeatedPatternUtility),
     genmoveWideRootNoise(genmoveWRN),
     analysisWideRootNoise(analysisWRN),
     genmoveAntiMirror(genmoveAntiMir),
     analysisAntiMirror(analysisAntiMir),
     nnEval(NULL),
     bot(NULL),
     currentRules(initialRules),
     params(initialParams),
     bTimeControls(),
     wTimeControls(),
     initialBoard(),
     initialPla(P_BLACK),
     moveHistory(),
     recentWinLossValues(),
     lastSearchFactor(1.0),
     desiredDynamicPDAForWhite(0.0),
     avoidMYTDaggerHack(avoidDagger),
     patternBonusTable(std::move(pbTable)),
     perspective(persp),
     genmoveTimeSum(0.0)
  {
  }

  ~GTPEngine() {
    stopAndWait();
    delete bot;
    delete nnEval;
  }

  void stopAndWait() {
    bot->stopAndWait();
  }

  Rules getCurrentRules() {
    return currentRules;
  }

  void clearStatsForNewGame() {
    //Currently nothing
  }

  //Specify -1 for the sizes for a default
  void setOrResetBoardSize(ConfigParser& cfg, Logger& logger, Rand& seedRand, int boardXSize, int boardYSize, bool loggingToStderr) {
    if(nnEval != NULL && boardXSize == nnEval->getNNXLen() && boardYSize == nnEval->getNNYLen())
      return;
    if(nnEval != NULL) {
      assert(bot != NULL);
      bot->stopAndWait();
      delete bot;
      delete nnEval;
      bot = NULL;
      nnEval = NULL;
      logger.write("Cleaned up old neural net and bot");
    }

    bool wasDefault = false;
    if(boardXSize == -1 || boardYSize == -1) {
      boardXSize = Board::DEFAULT_LEN;
      boardYSize = Board::DEFAULT_LEN;
      wasDefault = true;
    }

    const int maxConcurrentEvals = params.numThreads * 2 + 16; // * 2 + 16 just to give plenty of headroom
    const int expectedConcurrentEvals = params.numThreads;
    const int defaultMaxBatchSize = std::max(8,((params.numThreads+3)/4)*4);
    const bool defaultRequireExactNNLen = true;
    const string expectedSha256 = "";
    nnEval = Setup::initializeNNEvaluator(
      nnModelFile,nnModelFile,expectedSha256,cfg,logger,seedRand,maxConcurrentEvals,expectedConcurrentEvals,
      boardXSize,boardYSize,defaultMaxBatchSize,defaultRequireExactNNLen,
      Setup::SETUP_FOR_GTP
    );
    logger.write("Loaded neural net with nnXLen " + Global::intToString(nnEval->getNNXLen()) + " nnYLen " + Global::intToString(nnEval->getNNYLen()));

    {
      bool rulesWereSupported;
      nnEval->getSupportedRules(currentRules,rulesWereSupported);
      if(!rulesWereSupported) {
        throw StringError("Rules " + currentRules.toJsonStringNoKomi() + " from config file " + cfg.getFileName() + " are NOT supported by neural net");
      }
    }

    //On default setup, also override board size to whatever the neural net was initialized with
    //So that if the net was initalized smaller, we don't fail with a big board
    if(wasDefault) {
      boardXSize = nnEval->getNNXLen();
      boardYSize = nnEval->getNNYLen();
    }
    logger.write("Initializing board with boardXSize " + Global::intToString(boardXSize) + " boardYSize " + Global::intToString(boardYSize));
    if(!loggingToStderr)
      cerr << ("Initializing board with boardXSize " + Global::intToString(boardXSize) + " boardYSize " + Global::intToString(boardYSize)) << endl;

    string searchRandSeed;
    if(cfg.contains("searchRandSeed"))
      searchRandSeed = cfg.getString("searchRandSeed");
    else
      searchRandSeed = Global::uint64ToString(seedRand.nextUInt64());

    bot = new AsyncBot(params, nnEval, &logger, searchRandSeed);
    bot->setCopyOfExternalPatternBonusTable(patternBonusTable);

    Board board(boardXSize,boardYSize);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
    clearStatsForNewGame();
  }

  void setPositionAndRules(Player pla, const Board& board, const BoardHistory& h, const Board& newInitialBoard, Player newInitialPla, const vector<Move> newMoveHistory) {
    BoardHistory hist(h);
    //Ensure we always have this value correct
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);

    currentRules = hist.rules;
    bot->setPosition(pla,board,hist);
    initialBoard = newInitialBoard;
    initialPla = newInitialPla;
    moveHistory = newMoveHistory;
    recentWinLossValues.clear();
    updateDynamicPDA();
  }

  void clearBoard() {
    assert(bot->getRootHist().rules == currentRules);
    int newXSize = bot->getRootBoard().x_size;
    int newYSize = bot->getRootBoard().y_size;
    Board board(newXSize,newYSize);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
    clearStatsForNewGame();
  }

  bool setPosition(const vector<Move>& initialStones) {
    assert(bot->getRootHist().rules == currentRules);
    int newXSize = bot->getRootBoard().x_size;
    int newYSize = bot->getRootBoard().y_size;
    Board board(newXSize,newYSize);
    for(int i = 0; i<initialStones.size(); i++) {
      if(!board.isOnBoard(initialStones[i].loc) || board.colors[initialStones[i].loc] != C_EMPTY) {
        return false;
      }
      bool suc = board.setStone(initialStones[i].loc, initialStones[i].pla);
      if(!suc) {
        return false;
      }
    }

    //Make sure nothing died along the way
    for(int i = 0; i<initialStones.size(); i++) {
      if(board.colors[initialStones[i].loc] != initialStones[i].pla) {
        return false;
      }
    }
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    hist.setInitialTurnNumber(board.numStonesOnBoard()); //Heuristic to guess at what turn this is
    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
    clearStatsForNewGame();
    return true;
  }

  void updateKomiIfNew(float newKomi) {
    bot->setKomiIfNew(newKomi);
    currentRules.komi = newKomi;
  }

  void setStaticPlayoutDoublingAdvantage(double d) {
    staticPlayoutDoublingAdvantage = d;
    staticPDATakesPrecedence = true;
  }
  void setAnalysisWideRootNoise(double x) {
    analysisWideRootNoise = x;
  }
  void setRootPolicyTemperature(double x) {
    params.rootPolicyTemperature = x;
    bot->setParams(params);
    bot->clearSearch();
  }
  void setNumSearchThreads(int numThreads) {
    params.numThreads = numThreads;
    bot->setParams(params);
    bot->clearSearch();
  }

  void updateDynamicPDA() {
    updateDynamicPDAHelper(
      bot->getRootBoard(),bot->getRootHist(),
      dynamicPlayoutDoublingAdvantageCapPerOppLead,
      recentWinLossValues,
      desiredDynamicPDAForWhite
    );
  }

  bool play(Loc loc, Player pla) {
    assert(bot->getRootHist().rules == currentRules);
    bool suc = bot->makeMove(loc,pla,preventEncore);
    if(suc)
      moveHistory.push_back(Move(loc,pla));
    return suc;
  }

  bool undo() {
    if(moveHistory.size() <= 0)
      return false;
    assert(bot->getRootHist().rules == currentRules);

    vector<Move> moveHistoryCopy = moveHistory;

    Board undoneBoard = initialBoard;
    BoardHistory undoneHist(undoneBoard,initialPla,currentRules,0);
    undoneHist.setInitialTurnNumber(bot->getRootHist().initialTurnNumber);
    vector<Move> emptyMoveHistory;
    setPositionAndRules(initialPla,undoneBoard,undoneHist,initialBoard,initialPla,emptyMoveHistory);

    for(int i = 0; i<moveHistoryCopy.size()-1; i++) {
      Loc moveLoc = moveHistoryCopy[i].loc;
      Player movePla = moveHistoryCopy[i].pla;
      bool suc = play(moveLoc,movePla);
      assert(suc);
      (void)suc; //Avoid warning when asserts are off
    }
    return true;
  }

  bool setRulesNotIncludingKomi(Rules newRules, string& error) {
    assert(nnEval != NULL);
    assert(bot->getRootHist().rules == currentRules);
    newRules.komi = currentRules.komi;

    bool rulesWereSupported;
    nnEval->getSupportedRules(newRules,rulesWereSupported);
    if(!rulesWereSupported) {
      error = "Rules " + newRules.toJsonStringNoKomi() + " are not supported by this neural net version";
      return false;
    }

    vector<Move> moveHistoryCopy = moveHistory;

    Board board = initialBoard;
    BoardHistory hist(board,initialPla,newRules,0);
    hist.setInitialTurnNumber(bot->getRootHist().initialTurnNumber);
    vector<Move> emptyMoveHistory;
    setPositionAndRules(initialPla,board,hist,initialBoard,initialPla,emptyMoveHistory);

    for(int i = 0; i<moveHistoryCopy.size(); i++) {
      Loc moveLoc = moveHistoryCopy[i].loc;
      Player movePla = moveHistoryCopy[i].pla;
      bool suc = play(moveLoc,movePla);

      //Because internally we use a highly tolerant test, we don't expect this to actually trigger
      //even if a rules change did make some earlier moves illegal. But this check simply futureproofs
      //things in case we ever do
      if(!suc) {
        error = "Could not make the rules change, some earlier moves in the game would now become illegal.";
        return false;
      }
    }
    return true;
  }

  void ponder() {
    bot->ponder(lastSearchFactor);
  }

  struct AnalyzeArgs {
    bool analyzing = false;
    bool lz = false;
    bool kata = false;
    int minMoves = 0;
    int maxMoves = 10000000;
    bool showOwnership = false;
    bool showOwnershipStdev = false;
    bool showPVVisits = false;
    double secondsPerReport = TimeControls::UNLIMITED_TIME_DEFAULT;
    vector<int> avoidMoveUntilByLocBlack;
    vector<int> avoidMoveUntilByLocWhite;
  };

  void filterZeroVisitMoves(const AnalyzeArgs& args, vector<AnalysisData> buf) {
    //Avoid printing moves that have 0 visits, unless we need them
    //These should already be sorted so that 0-visit moves only appear at the end.
    int keptMoves = 0;
    for(int i = 0; i<buf.size(); i++) {
      if(buf[i].numVisits > 0 || keptMoves < args.minMoves)
        buf[keptMoves++] = buf[i];
    }
    buf.resize(keptMoves);
  }

  std::function<void(const Search* search)> getAnalyzeCallback(Player pla, AnalyzeArgs args, std::ostream& s_out) {
    std::function<void(const Search* search)> callback;
    //lz-analyze
    if(args.lz && !args.kata) {
      //Avoid capturing anything by reference except [this], since this will potentially be used
      //asynchronously and called after we return
      callback = [args,pla,&s_out,this](const Search* search) {
        vector<AnalysisData> buf;
        bool duplicateForSymmetries = true;
        search->getAnalysisData(buf,args.minMoves,false,analysisPVLen,duplicateForSymmetries);
        filterZeroVisitMoves(args,buf);
        if(buf.size() > args.maxMoves)
          buf.resize(args.maxMoves);
        if(buf.size() <= 0)
          return;

        const Board board = search->getRootBoard();
        for(int i = 0; i<buf.size(); i++) {
          if(i > 0)
            s_out << " ";
          const AnalysisData& data = buf[i];
          double winrate = 0.5 * (1.0 + data.winLossValue);
          double lcb = PlayUtils::getHackedLCBForWinrate(search,data,pla);
          if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
            winrate = 1.0-winrate;
            lcb = 1.0 - lcb;
          }
          s_out << "info";
          s_out << " move " << Location::toString(data.move,board);
          s_out << " visits " << data.numVisits;
          s_out << " winrate " << round(winrate * 10000.0);
          s_out << " prior " << round(data.policyPrior * 10000.0);
          s_out << " lcb " << round(lcb * 10000.0);
          s_out << " order " << data.order;
          s_out << " pv ";
          if(preventEncore && data.pvContainsPass())
            data.writePVUpToPhaseEnd(s_out,board,search->getRootHist(),search->getRootPla());
          else
            data.writePV(s_out,board);
          if(args.showPVVisits) {
            s_out << " pvVisits ";
            if(preventEncore && data.pvContainsPass())
              data.writePVVisitsUpToPhaseEnd(s_out,board,search->getRootHist(),search->getRootPla());
            else
              data.writePVVisits(s_out);
          }
        }
        s_out << endl;
      };
    }
    //kata-analyze, analyze (sabaki)
    else {
      callback = [args,pla,&s_out,this](const Search* search) {
        vector<AnalysisData> buf;
        bool duplicateForSymmetries = true;
        search->getAnalysisData(buf,args.minMoves,false,analysisPVLen,duplicateForSymmetries);
        filterZeroVisitMoves(args,buf);
        if(buf.size() > args.maxMoves)
          buf.resize(args.maxMoves);
        if(buf.size() <= 0)
          return;

        vector<double> ownership, ownershipStdev;
        if(args.showOwnershipStdev) {
          static constexpr int64_t ownershipStdevMinVisits = 3;
          tuple<vector<double>,vector<double>> ownershipAverageAndStdev;
          ownershipAverageAndStdev = search->getAverageAndStandardDeviationTreeOwnership(ownershipStdevMinVisits);
          ownership = std::get<0>(ownershipAverageAndStdev);
          ownershipStdev = std::get<1>(ownershipAverageAndStdev);
        }
        else if(args.showOwnership) {
          static constexpr int64_t ownershipMinVisits = 3;
          ownership = search->getAverageTreeOwnership(ownershipMinVisits);
        }

        ostringstream out;
        if(!args.kata) {
          //Hack for sabaki - ensure always showing decimal point. Also causes output to be more verbose with trailing zeros,
          //unfortunately, despite doing not improving the precision of the values.
          out << std::showpoint;
        }

        const Board board = search->getRootBoard();
        for(int i = 0; i<buf.size(); i++) {
          if(i > 0)
            out << " ";
          const AnalysisData& data = buf[i];
          double winrate = 0.5 * (1.0 + data.winLossValue);
          double utility = data.utility;
          //We still hack the LCB for consistency with LZ-analyze
          double lcb = PlayUtils::getHackedLCBForWinrate(search,data,pla);
          ///But now we also offer the proper LCB that KataGo actually uses.
          double utilityLcb = data.lcb;
          double scoreMean = data.scoreMean;
          double lead = data.lead;
          if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
            winrate = 1.0-winrate;
            lcb = 1.0 - lcb;
            utility = -utility;
            scoreMean = -scoreMean;
            lead = -lead;
            utilityLcb = -utilityLcb;
          }
          out << "info";
          out << " move " << Location::toString(data.move,board);
          out << " visits " << data.numVisits;
          out << " utility " << utility;
          out << " winrate " << winrate;
          out << " scoreMean " << lead;
          out << " scoreStdev " << data.scoreStdev;
          out << " scoreLead " << lead;
          out << " scoreSelfplay " << scoreMean;
          out << " prior " << data.policyPrior;
          out << " lcb " << lcb;
          out << " utilityLcb " << utilityLcb;
          if(data.isSymmetryOf != Board::NULL_LOC)
            out << " isSymmetryOf " << Location::toString(data.isSymmetryOf,board);
          out << " order " << data.order;
          out << " pv ";
          if(preventEncore && data.pvContainsPass())
            data.writePVUpToPhaseEnd(out,board,search->getRootHist(),search->getRootPla());
          else
            data.writePV(out,board);
          if(args.showPVVisits) {
            out << " pvVisits ";
            if(preventEncore && data.pvContainsPass())
              data.writePVVisitsUpToPhaseEnd(out,board,search->getRootHist(),search->getRootPla());
            else
              data.writePVVisits(out);
          }
        }

        if(args.showOwnership) {
          out << " ";

          out << "ownership";
          int nnXLen = search->nnXLen;
          for(int y = 0; y<board.y_size; y++) {
            for(int x = 0; x<board.x_size; x++) {
              int pos = NNPos::xyToPos(x,y,nnXLen);
              if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK))
                out << " " << -ownership[pos];
              else
                out << " " << ownership[pos];
            }
          }
        }

        if(args.showOwnershipStdev) {
          out << " ";

          out << "ownershipStdev";
          int nnXLen = search->nnXLen;
          for(int y = 0; y<board.y_size; y++) {
            for(int x = 0; x<board.x_size; x++) {
              int pos = NNPos::xyToPos(x,y,nnXLen);
              out << " " << ownershipStdev[pos];
            }
          }
        }

        s_out << out.str() << endl;
      };
    }
    return callback;
  }

  void genMove(
    Player pla,
    Logger& logger, double searchFactorWhenWinningThreshold, double searchFactorWhenWinning,
    enabled_t cleanupBeforePass, enabled_t friendlyPass, bool ogsChatToStderr,
    bool allowResignation, double resignThreshold, int resignConsecTurns, double resignMinScoreDifference,
    bool logSearchInfo, bool debug, bool playChosenMove,
    string& response, bool& responseIsError, bool& maybeStartPondering,
    AnalyzeArgs args, std::ostream& s_out
  ) {
    ClockTimer timer;

    response = "";
    responseIsError = false;
    maybeStartPondering = false;

    nnEval->clearStats();
    TimeControls tc = pla == P_BLACK ? bTimeControls : wTimeControls;

    //Update dynamic PDA given whatever the most recent values are, if we're using dynamic
    updateDynamicPDA();
    //Make sure we have the right parameters, in case someone ran analysis in the meantime.
    if(staticPDATakesPrecedence) {
      if(params.playoutDoublingAdvantage != staticPlayoutDoublingAdvantage) {
        params.playoutDoublingAdvantage = staticPlayoutDoublingAdvantage;
        bot->setParams(params);
      }
    }
    else {
      double desiredDynamicPDA =
        (params.playoutDoublingAdvantagePla == P_WHITE) ? desiredDynamicPDAForWhite :
        (params.playoutDoublingAdvantagePla == P_BLACK) ? -desiredDynamicPDAForWhite :
        (params.playoutDoublingAdvantagePla == C_EMPTY && pla == P_WHITE) ? desiredDynamicPDAForWhite :
        (params.playoutDoublingAdvantagePla == C_EMPTY && pla == P_BLACK) ? -desiredDynamicPDAForWhite :
        (assert(false),0.0);

      if(params.playoutDoublingAdvantage != desiredDynamicPDA) {
        params.playoutDoublingAdvantage = desiredDynamicPDA;
        bot->setParams(params);
      }
    }
    Player avoidMYTDaggerHackPla = avoidMYTDaggerHack ? pla : C_EMPTY;
    if(params.avoidMYTDaggerHackPla != avoidMYTDaggerHackPla) {
      params.avoidMYTDaggerHackPla = avoidMYTDaggerHackPla;
      bot->setParams(params);
    }
    if(params.wideRootNoise != genmoveWideRootNoise) {
      params.wideRootNoise = genmoveWideRootNoise;
      bot->setParams(params);
    }
    if(params.antiMirror != genmoveAntiMirror) {
      params.antiMirror = genmoveAntiMirror;
      bot->setParams(params);
    }

    {
      double avoidRepeatedPatternUtility = normalAvoidRepeatedPatternUtility;
      if(!args.analyzing) {
        double initialOppAdvantage = initialBlackAdvantage(bot->getRootHist()) * (pla == P_WHITE ? 1 : -1);
        if(initialOppAdvantage > getPointsThresholdForHandicapGame(getBoardSizeScaling(bot->getRootBoard())))
          avoidRepeatedPatternUtility = handicapAvoidRepeatedPatternUtility;
      }
      if(params.avoidRepeatedPatternUtility != avoidRepeatedPatternUtility) {
        params.avoidRepeatedPatternUtility = avoidRepeatedPatternUtility;
        bot->setParams(params);
      }
    }

    //Play faster when winning
    double searchFactor = PlayUtils::getSearchFactor(searchFactorWhenWinningThreshold,searchFactorWhenWinning,params,recentWinLossValues,pla);
    lastSearchFactor = searchFactor;

    Loc moveLoc;
    bot->setAvoidMoveUntilByLoc(args.avoidMoveUntilByLocBlack,args.avoidMoveUntilByLocWhite);
    if(args.analyzing) {
      std::function<void(const Search* search)> callback = getAnalyzeCallback(pla,args,s_out);
      if(args.showOwnership || args.showOwnershipStdev)
        bot->setAlwaysIncludeOwnerMap(true);
      else
        bot->setAlwaysIncludeOwnerMap(false);
      moveLoc = bot->genMoveSynchronousAnalyze(pla, tc, searchFactor, args.secondsPerReport, callback);
      //Make sure callback happens at least once
      callback(bot->getSearch());
    }
    else {
      moveLoc = bot->genMoveSynchronous(pla,tc,searchFactor);
    }

    bool isLegal = bot->isLegalStrict(moveLoc,pla);
    if(moveLoc == Board::NULL_LOC || !isLegal) {
      responseIsError = true;
      response = "genmove returned null location or illegal move";
      ostringstream sout;
      sout << "genmove null location or illegal move!?!" << "\n";
      sout << bot->getRootBoard() << "\n";
      sout << "Pla: " << PlayerIO::playerToString(pla) << "\n";
      sout << "MoveLoc: " << Location::toString(moveLoc,bot->getRootBoard()) << "\n";
      logger.write(sout.str());
      genmoveTimeSum += timer.getSeconds();
      return;
    }

    ReportedSearchValues values;
    double winLossValue;
    double lead;
    {
      values = bot->getSearch()->getRootValuesRequireSuccess();
      winLossValue = values.winLossValue;
      lead = values.lead;
    }

    //Record data for resignation or adjusting handicap behavior ------------------------
    recentWinLossValues.push_back(winLossValue);

    //Decide whether we should resign---------------------
    bool resigned = allowResignation && shouldResign(
      bot->getRootBoard(),bot->getRootHist(),pla,recentWinLossValues,lead,
      resignThreshold,resignConsecTurns,resignMinScoreDifference
    );


    //Snapshot the time NOW - all meaningful play-related computation time is done, the rest is just
    //output of various things.
    double timeTaken = timer.getSeconds();
    genmoveTimeSum += timeTaken;

    //Chatting and logging ----------------------------

    if(ogsChatToStderr) {
      int64_t visits = bot->getSearch()->getRootVisits();
      double winrate = 0.5 * (1.0 + (values.winValue - values.lossValue));
      double leadForPrinting = lead;
      //Print winrate from desired perspective
      if(perspective == P_BLACK || (perspective != P_BLACK && perspective != P_WHITE && pla == P_BLACK)) {
        winrate = 1.0 - winrate;
        leadForPrinting = -leadForPrinting;
      }
      cerr << "CHAT:"
           << "Visits " << visits
           << " Winrate " << Global::strprintf("%.2f%%", winrate * 100.0)
           << " ScoreLead " << Global::strprintf("%.1f", leadForPrinting)
           << " ScoreStdev " << Global::strprintf("%.1f", values.expectedScoreStdev);
      if(params.playoutDoublingAdvantage != 0.0) {
        cerr << Global::strprintf(
          " (PDA %.2f)",
          bot->getSearch()->getRootPla() == getOpp(params.playoutDoublingAdvantagePla) ?
          -params.playoutDoublingAdvantage : params.playoutDoublingAdvantage);
      }
      cerr << " PV ";
      bot->getSearch()->printPVForMove(cerr,bot->getSearch()->rootNode, moveLoc, analysisPVLen);
      cerr << endl;
    }

    if(logSearchInfo) {
      ostringstream sout;
      PlayUtils::printGenmoveLog(sout,bot,nnEval,moveLoc,timeTaken,perspective);
      logger.write(sout.str());
    }
    if(debug) {
      PlayUtils::printGenmoveLog(cerr,bot,nnEval,moveLoc,timeTaken,perspective);
    }

    //Hacks--------------------------------------------------
    //At least one of these hacks will use the bot to search stuff and clears its tree, so we apply them AFTER
    //all relevant logging and stuff.

    //Implement friendly pass - in area scoring rules other than tromp-taylor, maybe pass once there are no points
    //left to gain.
    int64_t numVisitsForFriendlyPass = 8 + std::min((int64_t)1000, std::min(params.maxVisits, params.maxPlayouts) / 10);
    moveLoc = PlayUtils::maybeFriendlyPass(cleanupBeforePass, friendlyPass, pla, moveLoc, bot->getSearchStopAndWait(), numVisitsForFriendlyPass);

    //Implement cleanupBeforePass hack - if the bot wants to pass, instead cleanup if there is something to clean
    //and we are in a ruleset where this is necessary or the user has configured it.
    moveLoc = PlayUtils::maybeCleanupBeforePass(cleanupBeforePass, friendlyPass, pla, moveLoc, bot);

    //Actual reporting of chosen move---------------------
    if(resigned)
      response = "resign";
    else
      response = Location::toString(moveLoc,bot->getRootBoard());

    if(!resigned && moveLoc != Board::NULL_LOC && isLegal && playChosenMove) {
      bool suc = bot->makeMove(moveLoc,pla,preventEncore);
      if(suc)
        moveHistory.push_back(Move(moveLoc,pla));
      assert(suc);
      (void)suc; //Avoid warning when asserts are off

      maybeStartPondering = true;
    }

    if(args.analyzing) {
      response = "play " + response;
    }

    return;
  }

  void clearCache() {
    bot->clearSearch();
    nnEval->clearCache();
  }

  void placeFixedHandicap(int n, string& response, bool& responseIsError) {
    int xSize = bot->getRootBoard().x_size;
    int ySize = bot->getRootBoard().y_size;
    Board board(xSize,ySize);
    try {
      PlayUtils::placeFixedHandicap(board,n);
    }
    catch(const StringError& e) {
      responseIsError = true;
      response = string(e.what()) + ", try place_free_handicap";
      return;
    }
    assert(bot->getRootHist().rules == currentRules);

    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);

    //Also switch the initial player, expecting white should be next.
    hist.clear(board,P_WHITE,currentRules,0);
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);
    hist.setInitialTurnNumber(board.numStonesOnBoard()); //Should give more accurate temperaure and time control behavior
    pla = P_WHITE;

    response = "";
    for(int y = 0; y<board.y_size; y++) {
      for(int x = 0; x<board.x_size; x++) {
        Loc loc = Location::getLoc(x,y,board.x_size);
        if(board.colors[loc] != C_EMPTY) {
          response += " " + Location::toString(loc,board);
        }
      }
    }
    response = Global::trim(response);
    (void)responseIsError;

    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
    clearStatsForNewGame();
  }

  void placeFreeHandicap(int n, string& response, bool& responseIsError, Rand& rand) {
    stopAndWait();

    //If asked to place more, we just go ahead and only place up to 30, or a quarter of the board
    int xSize = bot->getRootBoard().x_size;
    int ySize = bot->getRootBoard().y_size;
    int maxHandicap = xSize*ySize / 4;
    if(maxHandicap > 30)
      maxHandicap = 30;
    if(n > maxHandicap)
      n = maxHandicap;

    assert(bot->getRootHist().rules == currentRules);

    Board board(xSize,ySize);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,currentRules,0);
    double extraBlackTemperature = 0.25;
    PlayUtils::playExtraBlack(bot->getSearchStopAndWait(), n, board, hist, extraBlackTemperature, rand);
    //Also switch the initial player, expecting white should be next.
    hist.clear(board,P_WHITE,currentRules,0);
    hist.setAssumeMultipleStartingBlackMovesAreHandicap(assumeMultipleStartingBlackMovesAreHandicap);
    hist.setInitialTurnNumber(board.numStonesOnBoard()); //Should give more accurate temperaure and time control behavior
    pla = P_WHITE;

    response = "";
    for(int y = 0; y<board.y_size; y++) {
      for(int x = 0; x<board.x_size; x++) {
        Loc loc = Location::getLoc(x,y,board.x_size);
        if(board.colors[loc] != C_EMPTY) {
          response += " " + Location::toString(loc,board);
        }
      }
    }
    response = Global::trim(response);
    (void)responseIsError;

    vector<Move> newMoveHistory;
    setPositionAndRules(pla,board,hist,board,pla,newMoveHistory);
    clearStatsForNewGame();
  }

  void analyze(Player pla, AnalyzeArgs args, std::ostream& s_out) {
    assert(args.analyzing);
    //Analysis should ALWAYS be with the static value to prevent random hard-to-predict changes
    //for users.
    if(params.playoutDoublingAdvantage != staticPlayoutDoublingAdvantage) {
      params.playoutDoublingAdvantage = staticPlayoutDoublingAdvantage;
      bot->setParams(params);
    }
    if(params.avoidMYTDaggerHackPla != C_EMPTY) {
      params.avoidMYTDaggerHackPla = C_EMPTY;
      bot->setParams(params);
    }
    //Also wide root, if desired
    if(params.wideRootNoise != analysisWideRootNoise) {
      params.wideRootNoise = analysisWideRootNoise;
      bot->setParams(params);
    }
    if(params.antiMirror != analysisAntiMirror) {
      params.antiMirror = analysisAntiMirror;
      bot->setParams(params);
    }

    std::function<void(const Search* search)> callback = getAnalyzeCallback(pla,args,s_out);
    bot->setAvoidMoveUntilByLoc(args.avoidMoveUntilByLocBlack,args.avoidMoveUntilByLocWhite);
    if(args.showOwnership || args.showOwnershipStdev)
      bot->setAlwaysIncludeOwnerMap(true);
    else
      bot->setAlwaysIncludeOwnerMap(false);

    double searchFactor = 1e40; //go basically forever
    bot->analyzeAsync(pla, searchFactor, args.secondsPerReport, callback);
  }

  void computeAnticipatedWinnerAndScore(Player& winner, double& finalWhiteMinusBlackScore) {
    stopAndWait();

    //No playoutDoublingAdvantage to avoid bias
    //Also never assume the game will end abruptly due to pass
    {
      SearchParams tmpParams = params;
      tmpParams.playoutDoublingAdvantage = 0.0;
      tmpParams.conservativePass = true;
      bot->setParams(tmpParams);
    }

    //Make absolutely sure we can restore the bot's old state
    const Player oldPla = bot->getRootPla();
    const Board oldBoard = bot->getRootBoard();
    const BoardHistory oldHist = bot->getRootHist();

    Board board = bot->getRootBoard();
    BoardHistory hist = bot->getRootHist();
    Player pla = bot->getRootPla();

    //Tromp-taylorish scoring, or finished territory game scoring (including noresult)
    if(hist.isGameFinished && (
         (hist.rules.scoringRule == Rules::SCORING_AREA && !hist.rules.friendlyPassOk) ||
         (hist.rules.scoringRule == Rules::SCORING_TERRITORY)
       )
    ) {
      //For GTP purposes, we treat noResult as a draw since there is no provision for anything else.
      winner = hist.winner;
      finalWhiteMinusBlackScore = hist.finalWhiteMinusBlackScore;
    }
    //Human-friendly score or incomplete game score estimation
    else {
      int64_t numVisits = std::max(50, params.numThreads * 10);
      //Try computing the lead for white
      double lead = PlayUtils::computeLead(bot->getSearchStopAndWait(),NULL,board,hist,pla,numVisits,OtherGameProperties());

      //Round lead to nearest integer or half-integer
      if(hist.rules.gameResultWillBeInteger())
        lead = round(lead);
      else
        lead = round(lead+0.5)-0.5;

      finalWhiteMinusBlackScore = lead;
      winner = lead > 0 ? P_WHITE : lead < 0 ? P_BLACK : C_EMPTY;
    }

    //Restore
    bot->setPosition(oldPla,oldBoard,oldHist);
    bot->setParams(params);
  }

  vector<bool> computeAnticipatedStatuses() {
    stopAndWait();

    //Make absolutely sure we can restore the bot's old state
    const Player oldPla = bot->getRootPla();
    const Board oldBoard = bot->getRootBoard();
    const BoardHistory oldHist = bot->getRootHist();

    Board board = bot->getRootBoard();
    BoardHistory hist = bot->getRootHist();
    Player pla = bot->getRootPla();

    int64_t numVisits = std::max(100, params.numThreads * 20);
    vector<bool> isAlive;
    //Tromp-taylorish statuses, or finished territory game statuses (including noresult)
    if(hist.isGameFinished && (
         (hist.rules.scoringRule == Rules::SCORING_AREA && !hist.rules.friendlyPassOk) ||
         (hist.rules.scoringRule == Rules::SCORING_TERRITORY)
       )
    )
      isAlive = PlayUtils::computeAnticipatedStatusesSimple(board,hist);
    //Human-friendly statuses or incomplete game status estimation
    else {
      vector<double> ownershipsBuf;
      isAlive = PlayUtils::computeAnticipatedStatusesWithOwnership(bot->getSearchStopAndWait(),board,hist,pla,numVisits,ownershipsBuf);
    }

    //Restore
    bot->setPosition(oldPla,oldBoard,oldHist);

    return isAlive;
  }

  string rawNN(int whichSymmetry) {
    if(nnEval == NULL)
      return "";
    ostringstream out;

    for(int symmetry = 0; symmetry < SymmetryHelpers::NUM_SYMMETRIES; symmetry++) {
      if(whichSymmetry == NNInputs::SYMMETRY_ALL || whichSymmetry == symmetry) {
        Board board = bot->getRootBoard();
        BoardHistory hist = bot->getRootHist();
        Player nextPla = bot->getRootPla();

        MiscNNInputParams nnInputParams;
        nnInputParams.playoutDoublingAdvantage =
          (params.playoutDoublingAdvantagePla == C_EMPTY || params.playoutDoublingAdvantagePla == nextPla) ?
          staticPlayoutDoublingAdvantage : -staticPlayoutDoublingAdvantage;
        nnInputParams.symmetry = symmetry;
        NNResultBuf buf;
        bool skipCache = true;
        bool includeOwnerMap = true;
        nnEval->evaluate(board,hist,nextPla,nnInputParams,buf,skipCache,includeOwnerMap);

        NNOutput* nnOutput = buf.result.get();
        out << "symmetry " << symmetry << endl;
        out << "whiteWin " << Global::strprintf("%.6f",nnOutput->whiteWinProb) << endl;
        out << "whiteLoss " << Global::strprintf("%.6f",nnOutput->whiteLossProb) << endl;
        out << "noResult " << Global::strprintf("%.6f",nnOutput->whiteNoResultProb) << endl;
        out << "whiteLead " << Global::strprintf("%.3f",nnOutput->whiteLead) << endl;
        out << "whiteScoreSelfplay " << Global::strprintf("%.3f",nnOutput->whiteScoreMean) << endl;
        out << "whiteScoreSelfplaySq " << Global::strprintf("%.3f",nnOutput->whiteScoreMeanSq) << endl;
        out << "varTimeLeft " << Global::strprintf("%.3f",nnOutput->varTimeLeft) << endl;
        out << "shorttermWinlossError " << Global::strprintf("%.3f",nnOutput->shorttermWinlossError) << endl;
        out << "shorttermScoreError " << Global::strprintf("%.3f",nnOutput->shorttermScoreError) << endl;

        out << "policy" << endl;
        for(int y = 0; y<board.y_size; y++) {
          for(int x = 0; x<board.x_size; x++) {
            int pos = NNPos::xyToPos(x,y,nnOutput->nnXLen);
            float prob = nnOutput->policyProbs[pos];
            if(prob < 0)
              out << "    NAN ";
            else
              out << Global::strprintf("%8.6f ", prob);
          }
          out << endl;
        }
        out << "policyPass ";
        {
          int pos = NNPos::locToPos(Board::PASS_LOC,board.x_size,nnOutput->nnXLen,nnOutput->nnYLen);
          float prob = nnOutput->policyProbs[pos];
          if(prob < 0)
            out << "    NAN "; // Probably shouldn't ever happen for pass unles the rules change, but we handle it anyways
          else
            out << Global::strprintf("%8.6f ", prob);
          out << endl;
        }

        out << "whiteOwnership" << endl;
        for(int y = 0; y<board.y_size; y++) {
          for(int x = 0; x<board.x_size; x++) {
            int pos = NNPos::xyToPos(x,y,nnOutput->nnXLen);
            float whiteOwn = nnOutput->whiteOwnerMap[pos];
            out << Global::strprintf("%9.7f ", whiteOwn);
          }
          out << endl;
        }
        out << endl;
      }
    }

    return Global::trim(out.str());
  }

  SearchParams getParams() {
    return params;
  }

  void setParams(SearchParams p) {
    params = p;
    bot->setParams(params);
  }
};


int MainCmds::mctsserver(const vector<string>& args) {
  if(args.size() < 2) {
    cout << "Usage: mctsserver <port> [<max-connections>]" << endl;
    return 1;
  }
  int port = stoi(args[1]);
  int maxConnections = -1;
  if(args.size() > 2) {
    maxConnections = stoi(args[2]);
  }
  MctsServer server(port,maxConnections);
  server.run();
  return 0;
}