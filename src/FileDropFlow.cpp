// SPDX-License-Identifier: MIT
#include "FileDropFlow.h"
#include "ApplicationServices.h"
#include "ArchiveActions.h"
#include "Common.h"
#include "ExplicitSubmit.h"
#include "FileDrop.h"
#include "ImageConvert.h"
#include "InlineAction.h"
#include "JobSnapshot.h"
#include "MainWindow.h"
#include "OperationQueue.h"
#include "TransferCommand.h"
#include <stdexcept>

namespace Listi {

FileDropFlow::FileDropFlow(InlineAction &actionArea, OperationQueue &queue,
                           BMessenger owner, BMessenger service)
    : actionArea(actionArea), queue(queue), owner(owner), service(service) {}

bool FileDropFlow::Handle(const BMessage &message) {
  switch (message.what) {
  case kDrop: {
    if (actionArea.Pending()) {
      queue.ErrorInfo(
          "Finish or cancel the open question before dropping files.");
      break;
    }
    try {
      ++serial;
      missingParent.reset();
      auto job =
          PrepareFileDrop(message, message.GetString("destination", ""), true, &missingLeaf);
      if (!missingLeaf.empty()) {
        BMessage accepted(kDropAccepted);
        RequireJobField(accepted.AddInt64("request", serial));
        const auto path = job.destination + (job.destination == "/" ? "" : "/") + missingLeaf;
        const auto text = "Create this folder and copy " + std::to_string(job.sources.size()) +
                          " item(s) into it? Originals are kept.\n" + path;
        if (actionArea.Ask(text.c_str(), "Create and copy", accepted))
          missingParent = std::move(job);
        break;
      }
      if (job.action == Action::EditArchive) {
        auto prompt = ArchiveActions::ConfirmAddition(job, {});
        actionArea.Ask(prompt.text.String(), prompt.button, prompt.accepted);
        break;
      }
      const bool canConvert = CanProbeImageDrop(job);
      const bool sameFolder = InTransferSourceFolder(job);
      auto accepted = EncodeJob(job, kDropAccepted);
      RequireJobField(accepted.AddInt64("request", serial));
      if (sameFolder) {
        if (!canConvert)
          throw std::runtime_error(
              "Drop into a different folder, not the source folder.");
        RequireJobField(accepted.AddBool("convertOnly", true));
        actionArea.Ask(
            "Convert an image in its current folder. The original is kept.",
            "Convert here", accepted);
        break;
      }
      const bool canMove = CanMoveFileDrop(job);
      std::string text =
          "Drop " + std::to_string(job.sources.size()) + " item(s) into:\n" +
          job.destination +
          (!canMove
               ? "\nMove is only available on the same local volume. "
                 "Copy keeps originals."
               : "\nCopy keeps originals. Move removes originals after transfer.");
      actionArea.Ask(text.c_str(), "Copy here", accepted, nullptr,
                      canMove ? "Move here" : nullptr,
                      canConvert ? "Convert here" : nullptr);
    } catch (const std::exception &e) {
      PostMainWindowNotice(e.what());
    }
    break;
  }
  case kDropAccepted: {
    if (message.GetInt64("request", -1) != serial)
      break;
    const auto which = message.GetInt32("which", 0);
    if (missingParent) {
      auto parent = std::move(*missingParent);
      missingParent.reset();
      if (which == 1) {
        try {
          queue.Enqueue(CreateFileDropDestination(std::move(parent), missingLeaf));
        } catch (const std::exception &e) {
          PostMainWindowNotice(e.what());
        }
      }
      break;
    }
    if (which == 3 || message.GetBool("convertOnly", false)) {
      BMessage probe(message);
      probe.what = kImageDropProbe;
      probe.AddMessenger("replyTo", owner);
      if (service.SendMessage(&probe, (BHandler *)nullptr, 500000) != B_OK)
        PostMainWindowNotice("Could not check installed image translators.");
    } else if (which == 1 || which == 2) {
      auto job = DecodeSubmittedJob(message, {Action::Copy});
      if (!job)
        break;
      if (which == 2) {
        if (!CanMoveFileDrop(*job))
          break;
        job->action = Action::Move;
      }
      queue.Enqueue(*job);
    }
    break;
  }
  case kImageDropOutputs: {
    if (message.GetInt64("request", -1) != serial)
      break;
    if (actionArea.Pending()) {
      queue.ErrorInfo("Finish the open question and drop the image again.");
      break;
    }
    if (message.HasString("error")) {
      PostMainWindowNotice(message.GetString("error", ""));
      break;
    }
    auto job = DecodeSubmittedJob(message, {Action::Copy});
    if (!job || job->sources.size() != 1)
      break;
    std::vector<std::string> labels;
    BMessage output;
    for (int32 i = 0; message.FindMessage("output", i, &output) == B_OK; ++i) {
      std::string label = std::string(output.GetString("name", "")) + " (." +
                          output.GetString("extension", "") + ")";
      if (std::string(output.GetString("mime", "")) == "image/jpeg")
        label += " · transparency becomes white";
      labels.push_back(std::move(label));
    }
    if (labels.empty())
      break;
    BMessage accepted(message);
    accepted.what = kImageDropConvert;
    accepted.RemoveName("replyTo");
    accepted.RemoveName("which");
    std::string text = "Convert " + Leaf(job->sources.front()) +
                       " into:\n" + job->destination +
                       "\nThe original and existing files are kept.";
    actionArea.Ask(text.c_str(), "Convert", accepted, nullptr,
                   InTransferSourceFolder(*job) ? nullptr : "Copy original instead",
                   nullptr, labels);
    break;
  }
  case kImageDropConvert: {
    if (message.GetInt64("request", -1) != serial)
      break;
    if (message.GetInt32("which", 1) == 2) {
      auto job = DecodeSubmittedJob(message, {Action::Copy});
      if (job && !InTransferSourceFolder(*job))
        queue.Enqueue(*job);
      break;
    }
    BMessage output;
    const auto option = message.GetInt32("option", -1);
    if (option < 0 || message.FindMessage("output", option, &output) != B_OK)
      break;
    auto job = DecodeSubmittedJob(message, {Action::Copy});
    if (!job)
      break;
    job->action = Action::ConvertImage;
    job->imageFormat = output.GetInt32("type", 0);
    job->imageMime = output.GetString("mime", "");
    queue.Enqueue(*job);
    break;
  }
  default:
    return false;
  }
  return true;
}

} // namespace Listi
