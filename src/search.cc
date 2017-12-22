#include "defs.h"
#include "search.h"
#include "eval.h"
#include "movegen.h"
#include "transptable.h"
#include "movepicker.h"
#include "generalmovepicker.h"
#include "capturemovepicker.h"
#include <string>
#include <algorithm>
#include <time.h>
#include <iostream>
#include <chrono>

Search::Search(const Board& board, Limits limits, bool logUci) : 
  _orderingInfo(OrderingInfo(const_cast<TranspTable*>(&_tt))),
  _limits(limits),
  _board(board),
  _logUci(logUci),
  _stop(false),
  _limitCheckCount(0),
  _bestScore(0) {

  if (_limits.infinite) { // Infinite search
    _searchDepth = INF;
    _timeAllocated = INF;
  } else if (_limits.depth != 0) { // Depth search
    _searchDepth = _limits.depth;
    _timeAllocated = INF;
  } else if (_limits.time[_board.getActivePlayer()] != 0) { // Time search
    int timeRemaining = _limits.time[_board.getActivePlayer()] + _limits.increment[_board.getActivePlayer()];

    // If movestogo not specified, sudden death, assume SUDDEN_DEATH_MOVESTOGO moves remaining
    _timeAllocated = _limits.movesToGo == 0 ? timeRemaining / SUDDEN_DEATH_MOVESTOGO : timeRemaining / _limits.movesToGo;

    // Depth is infinity in a timed search (ends when time runs out)
    _searchDepth = MAX_SEARCH_DEPTH;
  } else { // No limits specified, use default depth
      _searchDepth = DEFAULT_SEARCH_DEPTH;
      _timeAllocated = INF;
  }
}

void Search::iterDeep() {
  _start = std::chrono::steady_clock::now();

  for (int currDepth=1;currDepth<=_searchDepth;currDepth++) {
    _rootMax(_board, currDepth);

    int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-_start).count();

    // If limits were exceeded in the search, break without logging UCI info (search was incomplete)
    if (_stop) break;

    if (_logUci) {
      _logUciInfo(_getPv(currDepth), currDepth, _bestMove, _bestScore, _nodes, elapsed);
    }

    // If the last search has exceeded or hit 50% of the allocated time, stop searching
    if (elapsed >= (_timeAllocated / 2)) break;
  }

  if (_logUci) std::cout << "bestmove " << getBestMove().getNotation() << std::endl;
}

MoveList Search::_getPv(int length) {
  MoveList pv;
  Board currBoard = _board;
  const TranspTableEntry* currEntry;
  int currLength = 0;

  while (currLength++ < length && (currEntry = _tt.getEntry(currBoard.getZKey()))) {
    pv.push_back(currEntry->getBestMove());
    currBoard.doMove(currEntry->getBestMove());
  }

  return pv;
}

void Search::_logUciInfo(const MoveList& pv, int depth, Move bestMove, int bestScore, int nodes, int elapsed) {
  std::string pvString;
  for(auto move : pv) {
    pvString += move.getNotation() + " ";
  }

  std::string scoreString;
  if (bestScore == INF) {
    scoreString = "mate " + std::to_string(pv.size());
  } else if (_bestScore == -INF) {
    scoreString = "mate -" + std::to_string(pv.size());
  } else {
    scoreString = "cp " + std::to_string(bestScore);
  }

  // Avoid divide by zero errors with nps
  elapsed++;

  std::cout << "info depth " + std::to_string(depth) + " ";
  std::cout << "nodes " + std::to_string(nodes) + " ";
  std::cout << "score " + scoreString + " ";
  std::cout << "nps " + std::to_string(nodes * 1000 / elapsed) + " ";
  std::cout << "time " + std::to_string(elapsed) + " ";
  std::cout << "pv " + pvString;
  std::cout << std::endl;
}

void Search::stop() {
  _stop = true;
}

Move Search::getBestMove() {
  return _bestMove;
}

int Search::getBestScore() {
  return _bestScore;
}

bool Search::_checkLimits() {
  if (--_limitCheckCount > 0) {
    return false;
  }

  _limitCheckCount = 4096;

  int elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-_start).count();

  if (_limits.nodes != 0 && (_nodes >= _limits.nodes)) return true;
  if (elapsed >= (_timeAllocated)) return true;

  return false;
}

void Search::_rootMax(const Board& board, int depth) {
  MoveGen movegen(board);
  MoveList legalMoves = movegen.getLegalMoves();
  _nodes = 0;

  // If no legal moves are avaliable, just return, setting bestmove to a null move
  if (legalMoves.size() == 0) {
    _bestMove = Move();
    _bestScore = -INF;
    return;
  }

  GeneralMovePicker movePicker(const_cast<OrderingInfo*>(&_orderingInfo), const_cast<Board*>(&board), const_cast<MoveList*>(&legalMoves));

  int alpha = -INF;
  int beta = INF;

  int currScore;

  Move bestMove;
  Board movedBoard;
  while (movePicker.hasNext()) {
    Move move = movePicker.getNext();
    movedBoard = board;
    movedBoard.doMove(move);

    _orderingInfo.incrementPly();
    currScore = -_negaMax(movedBoard, depth-1, -beta, -alpha);
    _orderingInfo.deincrementPly();

    if (_stop || _checkLimits()) {
      _stop = true;
      break;
    }

    // If the current score is better than alpha, or this is the first move in the loop
    if (currScore > alpha) {
      bestMove = move;
      alpha = currScore;

      // Break if we've found a checkmate
      if (currScore == INF) {
        break;
      }
    }
  }

  // If the best move was not set in the main search loop
  // alpha was not raised at any point, just pick the first move
  // avaliable (arbitrary) to avoid putting a null move in the
  // transposition table
  if (bestMove.getFlags() & Move::NULL_MOVE) {
    bestMove = legalMoves.at(0);
  }

  if (!_stop) {
    TranspTableEntry ttEntry(alpha, depth, TranspTableEntry::EXACT, bestMove);
    _tt.set(board.getZKey(), ttEntry);

    _bestMove = bestMove;
    _bestScore = alpha;
  }
}

int Search::_negaMax(const Board& board, int depth, int alpha, int beta) {
  // Check search limits
  if (_stop || _checkLimits()) {
    _stop = true;
    return 0;
  }

  int alphaOrig = alpha;

  const TranspTableEntry* ttEntry = _tt.getEntry(board.getZKey());
  // Check transposition table cache
  if (ttEntry && (ttEntry->getDepth() >= depth)) {
    switch(ttEntry->getFlag()) {
      case TranspTable::EXACT:
        return ttEntry->getScore();
      case TranspTable::UPPER_BOUND:
        beta = std::min(beta, ttEntry->getScore());
        break;
      case TranspTable::LOWER_BOUND:
        alpha = std::max(alpha, ttEntry->getScore());
        break;
    }

    if (alpha >= beta) {
      return ttEntry->getScore();
    }
  }

  // Transposition table lookups are inconclusive, generate moves and recurse
  MoveGen movegen(board);
  MoveList legalMoves = movegen.getLegalMoves();

  // Check for checkmate and stalemate
  if (legalMoves.size() == 0) {
    int score = board.colorIsInCheck(board.getActivePlayer()) ? -INF : 0; // -INF = checkmate, 0 = stalemate (draw)
    return score;
  }

  // Eval if depth is 0
  if (depth == 0) {
    return _qSearch(board, alpha, beta);
  }

  GeneralMovePicker movePicker(const_cast<OrderingInfo*>(&_orderingInfo), const_cast<Board*>(&board), const_cast<MoveList*>(&legalMoves));
  
  Move bestMove;
  Board movedBoard;
  while (movePicker.hasNext()) {
    Move move = movePicker.getNext();

    movedBoard = board;
    movedBoard.doMove(move);

    _orderingInfo.incrementPly();
    int score = -_negaMax(movedBoard, depth-1, -beta, -alpha);
    _orderingInfo.deincrementPly();
    
    // Beta cutoff
    if (score >= beta) {
      // Add this move as a new killer move and update history if move is quiet
      _orderingInfo.updateKillers(_orderingInfo.getPly(), move);
      if (!(move.getFlags() & Move::CAPTURE)) {
        _orderingInfo.incrementHistory(_board.getActivePlayer(), move.getFrom(), move.getTo(), depth);
      }

      // Add a new tt entry for this node
      TranspTableEntry newTTEntry(score, depth, TranspTableEntry::LOWER_BOUND, move);
      _tt.set(board.getZKey(), newTTEntry);
      return beta;
    }

    // Check if alpha raised (new best move)
    if (score > alpha) {
      alpha = score;
      bestMove = move;
    }
  }

  // If the best move was not set in the main search loop
  // alpha was not raised at any point, just pick the first move
  // avaliable (arbitrary) to avoid putting a null move in the
  // transposition table
  if (bestMove.getFlags() & Move::NULL_MOVE) {
    bestMove = legalMoves.at(0);
  }

  // Store bestScore in transposition table
  TranspTableEntry::Flag flag;
  if (alpha <= alphaOrig) {
    flag = TranspTableEntry::UPPER_BOUND;
  } else {
    flag = TranspTableEntry::EXACT;
  }
  TranspTableEntry newTTEntry(alpha, depth, flag, bestMove);
  _tt.set(board.getZKey(), newTTEntry);

  return alpha;
}

int Search::_qSearch(const Board& board, int alpha, int beta) {
  // Check search limits
  if (_stop || _checkLimits()) {
    _stop = true;
    return 0;
  }

  MoveGen movegen(board);
  MoveList legalMoves = movegen.getLegalMoves();

  // Check for checkmate / stalemate
  if (legalMoves.size() == 0) {
    if (board.colorIsInCheck(board.getActivePlayer())) { // Checkmate
      return -INF;
    } else { // Stalemate
      return 0;
    }
  }

  int standPat = Eval(board, board.getActivePlayer()).getScore();
  _nodes ++;

  CaptureMovePicker movePicker(const_cast<MoveList*>(&legalMoves));

  // If node is quiet, just return eval
  if (!movePicker.hasNext()) {
    return standPat;
  }

  if (standPat >= beta) {
    return beta;
  }
  if (alpha < standPat) {
    alpha = standPat;
  }

  Board movedBoard;
  while (movePicker.hasNext()) {
    Move move = movePicker.getNext();

    movedBoard = board;
    movedBoard.doMove(move);

    int score = -_qSearch(movedBoard, -beta, -alpha);

    if (score >= beta) {
      return beta;
    }
    if (score > alpha) {
      alpha = score;
    }
  }
  return alpha;
}
