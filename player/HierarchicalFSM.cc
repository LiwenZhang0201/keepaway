//
// Created by baj on 10/3/16.
//

#include "HierarchicalFSM.h"
#include "ChoicePoint.h"

namespace fsm {

Memory::Memory() {
  bAlive = true;

  resetState();
}

Memory &Memory::ins() {
  static Memory memory;
  return memory;
}

void Memory::resetState() {
  memset(state, 0, sizeof(state));
  memset(K, 0, sizeof(K));
}

string Memory::to_string() {
  stringstream ss;
  PRINT_VALUE_STREAM(ss, agentIdx);
  PRINT_VALUE_STREAM(ss, vector<ObjectT>(K, K + HierarchicalFSM::num_keepers));
  PRINT_VALUE_STREAM(
      ss, vector<double>(state, state + HierarchicalFSM::num_features));
  PRINT_VALUE_STREAM(ss, stack);

  return ss.str();
}

const vector<string> &Memory::getStack() const { return stack; }

void Memory::PushStack(const string &s) {
  stack.push_back(s);
  if (Log.isInLogLevel(101)) {
    Log.log(101, "Memory::PushStack: %s", to_prettystring(stack).c_str());
  }
}

void Memory::PopStack() {
  stack.pop_back();
  if (Log.isInLogLevel(101)) {
    Log.log(101, "Memory::PopStack: %s", to_prettystring(stack).c_str());
  }
}

int HierarchicalFSM::num_features;
int HierarchicalFSM::num_keepers;

HierarchicalFSM::HierarchicalFSM(BasicPlayer *p, const std::string &name)
    : player(p), name(name) {
  ACT = player->ACT;
  WM = player->WM;
  SS = player->SS;
  PS = player->PS;

  dummyChoice = new ChoicePoint<int>("}", {0});
}

HierarchicalFSM::~HierarchicalFSM() { delete dummyChoice; }

void HierarchicalFSM::action(bool sync) {
  Log.log(101, "action with stack=%s", getStackStr().c_str());

  while (Memory::ins().bAlive) {
    if (sync)
      MakeChoice<int>(dummyChoice).operator()(WM->getCurrentCycle());

    if (WM->getTimeLastSeeMessage() == WM->getCurrentTime() ||
        (SS->getSynchMode() && WM->getRecvThink())) {
      Log.log(101, "send commands");
      ACT->sendCommands();

      if (SS->getSynchMode()) {
        WM->processRecvThink(false);
        ACT->sendMessageDirect("(done)");
      }
    }

    Memory::ins().bAlive = WM->waitForNewInformation();
    if (!Memory::ins().bAlive)
      break;
    Log.setHeader(WM->getCurrentCycle());
    WM->updateAll();

    auto e = getState();
    if (e.length()) {
      idle(e);
    } else {
      break;
    }
  }
}

void HierarchicalFSM::idle(const std::string error) {
  SoccerCommand soc;
  if (error != "ball lost") {
    ACT->putCommandInQueue(soc = player->turnBodyToObject(OBJECT_BALL));
    ACT->putCommandInQueue(player->turnNeckToObject(OBJECT_BALL, soc));
  }
  Log.log(101, "idle (error: %s)", error.c_str());
}

string HierarchicalFSM::getState() {
  Memory::ins().resetState();

  if (WM->getConfidence(OBJECT_BALL) < PS->getBallConfThr()) {
    ACT->putCommandInQueue(player->searchBall());
    ACT->putCommandInQueue(player->alignNeckWithBody());
    return "ball lost";
  }

  int numK = WM->getNumKeepers();
  Assert(numK == num_keepers);

  int features = WM->keeperStateVars(Memory::ins().state);
  Assert(features == 0 || features == num_features);
  if (features != num_features)
    return "features != SA->getNumFeatures()";

  for (int i = 0; i < numK; i++)
    Memory::ins().K[i] = SoccerTypes::getTeammateObjectFromIndex(i);

  ObjectT K0 = WM->getClosestInSetTo(OBJECT_SET_TEAMMATES, OBJECT_BALL);
  if (!WM->sortClosestTo(Memory::ins().K, numK, K0))
    return "!WM->sortClosestTo(K, numK, K0)";
  if (K0 != Memory::ins().K[0])
    return "K0 != K[0]";

  auto &agentIdx = Memory::ins().agentIdx;
  agentIdx = 0;
  while (agentIdx < numK &&
         Memory::ins().K[agentIdx] != WM->getAgentObjectType())
    agentIdx += 1;
  Assert(agentIdx < numK);

  if (agentIdx >= numK)
    return "agentIdx >= numK";
  return "";
}

const string &HierarchicalFSM::getName() const { return name; }

bool HierarchicalFSM::running() {
  return Memory::ins().bAlive && !WM->isNewEpisode();
}

string HierarchicalFSM::getStackStr() {
  string str;
  for (auto &s : Memory::ins().getStack())
    str += s + " ";
  return str;
}

void HierarchicalFSM::initialize(int numFeatures, int numKeepers, bool bLearn,
                                 double widths[], double Gamma, double Lambda,
                                 double initialWeight, bool qLearning,
                                 string loadWeightsFile,
                                 string saveWeightsFile) {
  num_features = numFeatures;
  num_keepers = numKeepers;
  LinearSarsaLearner::ins().initialize(bLearn, widths, Gamma, Lambda,
                                       initialWeight, qLearning,
                                       loadWeightsFile, saveWeightsFile);
}

Keeper::Keeper(BasicPlayer *p) : HierarchicalFSM(p, "{") {
  pass = new Pass(p);
  hold = new Hold(p);
  move = new Move(p);
  stay = new Stay(p);
  intercept = new Intercept(p);

  choices[0] = new ChoicePoint<HierarchicalFSM *>("@Free", {stay, move});
  choices[1] = new ChoicePoint<HierarchicalFSM *>("@Ball", {pass, hold});
}

Keeper::~Keeper() {
  delete choices[0];
  delete choices[1];

  delete pass;
  delete hold;
  delete move;
  delete stay;
  delete intercept;
}

void Keeper::run() {
  while (WM->getCurrentCycle() < 1)
    action(false);

  while (Memory::ins().bAlive) {
    if (WM->isNewEpisode()) {
      LinearSarsaLearner::ins().endEpisode(WM->getCurrentCycle());
      WM->setNewEpisode(false);
    }

    Log.log(101, "Keeper::run agentIdx %d", Memory::ins().agentIdx);
    if (!WM->isBallKickable() && !WM->isTmControllBall()) { // ball free
      int minCycle = INT_MAX;
      int agentCycle = INT_MAX;
      for (int i = 0; i < WM->getNumKeepers(); ++i) {
        int iCycle = INT_MAX;
        auto o = Memory::ins().K[i];
        SoccerCommand illegal;
        WM->predictCommandToInterceptBall(o, illegal, &iCycle);
        minCycle = min(minCycle, iCycle);
        if (o == WM->getAgentObjectType())
          agentCycle = iCycle;
        Log.log(101, "Keeper::run ballFree teammate idx %d iCycle %d", i,
                iCycle);
      }

      if (agentCycle == minCycle) {
        Log.log(101, "Keeper::run ballFree agentCycle %d = minCycle %d",
                agentCycle, minCycle);
        Run(intercept).operator()();
      } else {
        MakeChoice<HierarchicalFSM *> c(choices[0]);
        auto m = c(WM->getCurrentCycle());
        Run(m).operator()();
      }
    } else {
      if (WM->isBallKickable()) {
        MakeChoice<HierarchicalFSM *> c(choices[1]);
        auto m = c(WM->getCurrentCycle());
        Run(m).operator()();
      } else {
        MakeChoice<HierarchicalFSM *> c(choices[0]);
        auto m = c(WM->getCurrentCycle());
        Run(m).operator()();
      }
    }
  }

  Log.log(101, "Keeper::run exit");
}

Move::Move(BasicPlayer *p) : HierarchicalFSM(p, "$Move") {
  moveToChoice = new ChoicePoint<int>("@MoveTo", {0, 1, 2, 3});
  moveSpeedChoice = new ChoicePoint<double>(
      "@MoveSpeed", {SS->getPlayerSpeedMax() / 2.0, SS->getPlayerSpeedMax()});
}

Move::~Move() {
  delete moveToChoice;
  delete moveSpeedChoice;
}

void Move::run() {
  auto c = makeComposedChoice(moveToChoice, moveSpeedChoice);
  int dir;
  double speed;
  tie(dir, speed) = c->operator()(WM->getCurrentCycle());

  bool flag = WM->isTmControllBall();
  while (running() && flag == WM->isTmControllBall()) {
    SoccerCommand soc;
    VecPosition target = WM->getAgentGlobalPosition() +
                         (WM->getBallPos() - WM->getAgentGlobalPosition())
                             .rotate(dir * 90.0)
                             .normalize();

    auto r = WM->getKeepawayRectReduced();
    if (r.isInside(target)) {
      auto distance = target.getDistanceTo(WM->getAgentGlobalPosition());
      auto cycles = rint(distance / speed);
      ACT->putCommandInQueue(
          soc = player->moveToPos(target, 30.0, 1.0, false, (int) cycles));
    } else {
      double minDist = std::numeric_limits<double>::max();
      VecPosition refinedTarget;
      pair<VecPosition, VecPosition> pt[4];

      pt[0] = {r.getPosLeftTop(), r.getPosRightTop()};
      pt[1] = {r.getPosRightTop(), r.getPosRightBottom()};
      pt[2] = {r.getPosRightBottom(), r.getPosLeftBottom()};
      pt[3] = {r.getPosLeftBottom(), r.getPosLeftTop()};

      for (int i = 0; i < 4; ++i) {
        auto edge = Line::makeLineFromTwoPoints(pt[i].first, pt[i].second);
        auto refined = edge.getPointOnLineClosestTo(target);
        bool inBetween = edge.isInBetween(refined, pt[i].first, pt[i].second);
        if (inBetween) {
          auto dist = refined.getDistanceTo(target);
          if (dist < minDist) {
            minDist = dist;
            refinedTarget = refined;
          }
        }
      }

      if (minDist < std::numeric_limits<double>::max()) {
        auto distance = target.getDistanceTo(WM->getAgentGlobalPosition());
        auto cycles = rint(distance / speed);
        ACT->putCommandInQueue(
            soc = player->moveToPos(refinedTarget, 30.0, 1.0, false, (int) cycles));
      } else {
        ACT->putCommandInQueue(soc = player->turnBodyToObject(OBJECT_BALL));
        ACT->putCommandInQueue(player->turnNeckToObject(OBJECT_BALL, soc));
      }
    }

    ACT->putCommandInQueue(player->turnNeckToObject(OBJECT_BALL, soc));
    Log.log(101, "Move::run action with dir=%d speed=%f", dir, speed);
    Action(this, {to_string(dir), to_string(speed)})();
  }
}

Stay::Stay(BasicPlayer *p) : HierarchicalFSM(p, "$Stay") {}

void Stay::run() {
  bool flag = WM->isTmControllBall();
  while (running() && flag == WM->isTmControllBall()) {
    SoccerCommand soc;
    ACT->putCommandInQueue(soc = player->turnBodyToObject(OBJECT_BALL));
    ACT->putCommandInQueue(player->turnNeckToObject(OBJECT_BALL, soc));
    Log.log(101, "Stay::run action");
    Action(this)();
  }
}

Intercept::Intercept(BasicPlayer *p) : HierarchicalFSM(p, "$Intercept") {}

void Intercept::run() {
  while (running() && !WM->isTmControllBall()) {
    SoccerCommand soc;
    ACT->putCommandInQueue(soc = player->intercept(false));
    ACT->putCommandInQueue(player->turnNeckToObject(OBJECT_BALL, soc));
    Log.log(101, "Intercept::run action");
    Action(this)();
  }
}

Pass::Pass(BasicPlayer *p) : HierarchicalFSM(p, "$Pass") {
  vector<int> parameters;
  for (int i = 1; i < num_keepers; ++i) {
    parameters.push_back(i);
  }
  passToChoice = new ChoicePoint<int>("@PassTo", parameters);
  passSpeedChoice =
      new ChoicePoint<PassT>("@PassSpeed", {PASS_SLOW, PASS_NORMAL, PASS_FAST});
}

Pass::~Pass() {
  delete passToChoice;
  delete passSpeedChoice;
}

void Pass::run() {
  auto c = makeComposedChoice(passToChoice, passSpeedChoice);
  int teammate;
  PassT speed;
  tie(teammate, speed) = c->operator()(WM->getCurrentCycle());

  while (running() && WM->isBallKickable()) {
    VecPosition tmPos = WM->getGlobalPosition(Memory::ins().K[teammate]);
    VecPosition tmVel = WM->getGlobalVelocity(Memory::ins().K[teammate]);
    VecPosition target = WM->predictFinalAgentPos(&tmPos, &tmVel);
    ACT->putCommandInQueue(player->directPass(target, speed));
    Log.log(101, "Pass::run action with teammate=%d speed=%s", teammate,
            to_prettystring(speed).c_str());
    Action(this, {to_string(teammate), to_string(speed)})();
  }
}

Hold::Hold(BasicPlayer *p) : HierarchicalFSM(p, "$Hold") {}

Hold::~Hold() {}

void Hold::run() {
  SoccerCommand soc;
  ACT->putCommandInQueue(soc = player->holdBall());
  ACT->putCommandInQueue(player->turnNeckToObject(OBJECT_BALL, soc));
  Action(this)();
}

}
