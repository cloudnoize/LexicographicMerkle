#include "tree.hpp"

namespace merkle {
void Tree::insert(ByteSequence&& key, ByteSequence&& value) {
    auto* branchNode = root_.get();
    ExtensionView extension{key};
    while (true) {
        auto [result, matchBytes] = extension.compareTo(branchNode->extension());
        if (result == ExtensionView::CompareResultType::equals) {
            branchNode->setLeaf(key, value);
            return;
        } else if (result == ExtensionView::CompareResultType::contains_other_extension) {
            // this means that the current branch node is on the path.
            extension.incrementPositionBy(matchBytes);
            auto optCurrentByte = extension.getCurrentByte();
            assert(optCurrentByte.has_value());
            auto currentByte = *optCurrentByte;
            auto nodeType = branchNode->getTypeOfChild(currentByte);
            extension.incrementPositionBy(1);
            if (nodeType == Node::Type::NullNode) {
                // Add the a leaf and set extension
                auto leafExtension = extension.getExtentionFromCurrentPosition();
                auto newLeaf = HashOfLeaf::createhashOfLeaf(
                    key, value, ByteSequence{leafExtension.begin(), leafExtension.end()});
                branchNode->swapNodeAtChild(currentByte, newLeaf);
                return;
            } else if (nodeType == Node::Type::HashOfLeaf) {
                // We have a leaf on the path, need to compare its extension to deduce the action.
                auto [result, matchBytes] =
                    extension.compareTo(branchNode->getChildAt(currentByte)->extension());
                if (result == ExtensionView::CompareResultType::equals) {
                    // it's an update
                    branchNode->updateHashOfLeafChild(currentByte, key, value);
                    return;
                }
                // the new inserted key and the existing leaf, shares a common path i.e. we need to
                // insert a new branch node.
                // the key in the db to the new branch node
                auto newBranchNodeKey = extension.getKeySoFar();
                // the common extension is the extension of the new branch node.
                auto newExtensionView = extension.getExtentionFromCurrentPositionUntil(matchBytes);
                std::unique_ptr<Node> nodeToMove;
                branchNode->swapNodeAtChild(currentByte, nodeToMove);
                nodeToMove->truncateExtension(matchBytes);
                auto byteToSetHashOfLeaf = ExtensionView{nodeToMove->extension()}.getCurrentByte();
                nodeToMove->truncateExtension(1);
                std::vector<BranchNode::ChildAndPos> cnps;
                cnps.emplace_back(std::make_pair(std::ref(nodeToMove), byteToSetHashOfLeaf));
                // New leaf preparation
                // get to the next byte after the common extension
                extension.incrementPositionBy(matchBytes);
                auto optNewLeafCurrentByte = extension.getCurrentByte();
                // if current byte is nullopt it means that its path terminates at this node
                extension.incrementPositionBy(1);
                auto hashofleaf = HashOfLeaf::createhashOfLeaf(
                    key, value,
                    ByteSequence{extension.getExtentionFromCurrentPosition().begin(),
                                 extension.getExtentionFromCurrentPosition().end()});
                cnps.emplace_back(std::make_pair(std::ref(hashofleaf), optNewLeafCurrentByte));

                auto newBranchNode = BranchNode::createBranchNode(
                    ByteSequence{newExtensionView.begin(), newExtensionView.end()}, cnps);
                // this will create the dirty hashof branch for this node, and will swap with the
                // hashofleaf
                auto nodeToSwap = newBranchNode->createHashOfBranchForThisNode();
                branchNode->swapNodeAtChild(currentByte, nodeToSwap);

                db_.emplace(ByteSequence{newBranchNodeKey.begin(), newBranchNodeKey.end()},
                            newBranchNode.release());
                return;
            } else if (nodeType == Node::Type::HashOfBranch) {
                // TODO This is the only place we go to the next iteration. we need to stack the prv
                // node as we might need to change its hashOfBranch. can we do optimization to skip
                // reading the next node if the extension of the hashofbranch shows that we can skip
                // it? it can contradict the fact that we may need to change the hashof branch
                // extension. so we need hashofbranch exntesion?
                auto nextbranchDbKey = extension.getKeySoFar();
                branchNode->setDirty(currentByte);
                branchNode = getMutableBranchNode(nextbranchDbKey).get();
                assert(branchNode != nullptr);
            } else {
                // TODO , we should not reach here, assert;
            }
        } else if (result == ExtensionView::CompareResultType::substring) {
            // Prepare the new branch node with a leaf child and a hashofbranch to the current
            // branch node
            auto currentBranchDbKey = extension.getKeySoFar();
            auto newExtensionView = extension.getExtentionFromCurrentPositionUntil(matchBytes);
            extension.incrementPositionBy(matchBytes);
            auto leafPos = extension.getCurrentByte();
            assert(leafPos == std::nullopt);
            extension.incrementPositionBy(1);
            auto hashofleaf = HashOfLeaf::createhashOfLeaf(
                key, value,
                ByteSequence{extension.getExtentionFromCurrentPosition().begin(),
                             extension.getExtentionFromCurrentPosition().end()});
            std::vector<BranchNode::ChildAndPos> cnps;
            cnps.emplace_back(std::make_pair(std::ref(hashofleaf), leafPos));
            // truncate the extension of the older branchnode
            branchNode->truncateExtension(matchBytes);
            Byte nextByte = branchNode->extension()[0];
            branchNode->truncateExtension(1);
            // set the hashofBranch to point to the old branchnode
            auto hashOfBranch = branchNode->createHashOfBranchForThisNode();
            cnps.emplace_back(std::make_pair(std::ref(hashOfBranch), nextByte));
            auto newBranchNode = BranchNode::createBranchNode(
                ByteSequence{newExtensionView.begin(), newExtensionView.end()}, cnps);

            // At this phase the new branch node is ready with the leaf and pointing to the old
            // branch node, need to set it in the db with the key of the old branchnode
            auto& mutableBranchNode = getMutableBranchNode(currentBranchDbKey);
            mutableBranchNode.swap(newBranchNode);
            auto newDbKeyView = extension.getWholeExtension();
            auto newDbKEy = ByteSequence{newDbKeyView.begin(), newDbKeyView.end()};
            newDbKEy.push_back(nextByte);
            db_.emplace(std::move(newDbKEy), newBranchNode.release());
            return;
        } else if (result == ExtensionView::CompareResultType::diverge) {
            // TODO new branch node with the common extension, insert hash of leaf with extension
            // for the new key/value,
            auto newBranchDbKey =
                extension.getExtentionRange(0, extension.getPosition() + matchBytes);

            auto currentBranchDbKey = extension.getKeySoFar();
            auto commonExtensionView = extension.getExtentionFromCurrentPositionUntil(matchBytes);
            // As this branchnode is inserted in the middle of the prv branch node and the current,
            // we need to set the prv branchnode hashof branch

            extension.incrementPositionBy(matchBytes);

            auto optCurrentByte = extension.getCurrentByte();
            extension.incrementPositionBy(1);

            auto hashofleaf = HashOfLeaf::createhashOfLeaf(
                key, value,
                ByteSequence{extension.getExtentionFromCurrentPosition().begin(),
                             extension.getExtentionFromCurrentPosition().end()});
            std::vector<BranchNode::ChildAndPos> cnps;
            cnps.emplace_back(std::make_pair(std::ref(hashofleaf), optCurrentByte));

            // truncate the extension of the older branchnode
            branchNode->truncateExtension(matchBytes);
            Byte nextByte = branchNode->extension()[0];
            branchNode->truncateExtension(1);

            // set the hashofBranch to point to the old branchnode
            auto hashOfBranch = branchNode->createHashOfBranchForThisNode();
            cnps.emplace_back(std::make_pair(std::ref(hashOfBranch), nextByte));

            auto newBranchNode = BranchNode::createBranchNode(
                ByteSequence{commonExtensionView.begin(), commonExtensionView.end()}, cnps);

            // At this phase the new branch node is ready with the leaf and pointing to the old
            // branch node, need to set it in the db with the key of the old branchnode
            auto& mutableBranchNode = getMutableBranchNode(currentBranchDbKey);
            mutableBranchNode.swap(newBranchNode);
            auto newDbKEy = ByteSequence{newBranchDbKey.begin(), newBranchDbKey.end()};
            newDbKEy.push_back(nextByte);
            db_.emplace(std::move(newDbKEy), newBranchNode.release());
            return;
        }
    }
}

};  // namespace merkle
