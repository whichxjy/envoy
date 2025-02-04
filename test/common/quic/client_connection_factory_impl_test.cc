#include <chrono>

#include "source/common/quic/client_connection_factory_impl.h"
#include "source/common/quic/quic_transport_socket_factory.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/alternate_protocols_cache.h"
#include "test/mocks/server/transport_socket_factory_context.h"
#include "test/mocks/ssl/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/test_common/environment.h"
#include "test/test_common/network_utility.h"
#include "test/test_common/simulated_time_system.h"

#include "quiche/quic/core/crypto/quic_client_session_cache.h"

using testing::Return;

namespace Envoy {
namespace Quic {

class QuicNetworkConnectionTest : public Event::TestUsingSimulatedTime,
                                  public testing::TestWithParam<Network::Address::IpVersion> {
protected:
  void initialize() {
    EXPECT_CALL(*cluster_, perConnectionBufferLimitBytes()).WillOnce(Return(45));
    EXPECT_CALL(*cluster_, connectTimeout).WillOnce(Return(std::chrono::seconds(10)));
    auto* protocol_options = cluster_->http3_options_.mutable_quic_protocol_options();
    protocol_options->mutable_max_concurrent_streams()->set_value(43);
    protocol_options->mutable_initial_stream_window_size()->set_value(65555);
    quic_info_ = createPersistentQuicInfoForCluster(dispatcher_, *cluster_);
    EXPECT_EQ(quic_info_->quic_config_.max_time_before_crypto_handshake(),
              quic::QuicTime::Delta::FromSeconds(10));
    EXPECT_EQ(quic_info_->quic_config_.GetMaxBidirectionalStreamsToSend(),
              protocol_options->max_concurrent_streams().value());
    EXPECT_EQ(quic_info_->quic_config_.GetMaxUnidirectionalStreamsToSend(),
              protocol_options->max_concurrent_streams().value());
    EXPECT_EQ(quic_info_->quic_config_.GetInitialMaxStreamDataBytesIncomingBidirectionalToSend(),
              protocol_options->initial_stream_window_size().value());

    test_address_ = Network::Utility::resolveUrl(
        absl::StrCat("tcp://", Network::Test::getLoopbackAddressUrlString(GetParam()), ":30"));
    Ssl::ClientContextSharedPtr context{new Ssl::MockClientContext()};
    EXPECT_CALL(context_.context_manager_, createSslClientContext(_, _)).WillOnce(Return(context));
    factory_ = std::make_unique<Quic::QuicClientTransportSocketFactory>(
        std::unique_ptr<Envoy::Ssl::ClientContextConfig>(
            new NiceMock<Ssl::MockClientContextConfig>),
        context_);
    crypto_config_ = std::make_shared<quic::QuicCryptoClientConfig>(
        std::make_unique<Quic::EnvoyQuicProofVerifier>(factory_->sslCtx()),
        std::make_unique<quic::QuicClientSessionCache>());
  }

  uint32_t highWatermark(EnvoyQuicClientSession* session) {
    return session->write_buffer_watermark_simulation_.highWatermark();
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  std::unique_ptr<PersistentQuicInfoImpl> quic_info_;
  std::shared_ptr<Upstream::MockClusterInfo> cluster_{new NiceMock<Upstream::MockClusterInfo>()};
  Upstream::HostSharedPtr host_{new NiceMock<Upstream::MockHost>};
  NiceMock<Random::MockRandomGenerator> random_;
  Upstream::ClusterConnectivityState state_;
  Network::Address::InstanceConstSharedPtr test_address_;
  NiceMock<Server::Configuration::MockTransportSocketFactoryContext> context_;
  std::unique_ptr<Quic::QuicClientTransportSocketFactory> factory_;
  std::shared_ptr<quic::QuicCryptoClientConfig> crypto_config_;
  Stats::IsolatedStoreImpl store_;
  QuicStatNames quic_stat_names_{store_.symbolTable()};
};

TEST_P(QuicNetworkConnectionTest, BufferLimits) {
  initialize();

  const int port = 30;
  std::unique_ptr<Network::ClientConnection> client_connection = createQuicNetworkConnection(
      *quic_info_, crypto_config_,
      quic::QuicServerId{factory_->clientContextConfig().serverNameIndication(), port, false},
      dispatcher_, test_address_, test_address_, quic_stat_names_, {}, store_);
  EnvoyQuicClientSession* session = static_cast<EnvoyQuicClientSession*>(client_connection.get());
  session->Initialize();
  client_connection->connect();
  EXPECT_TRUE(client_connection->connecting());
  ASSERT(session != nullptr);
  EXPECT_EQ(highWatermark(session), 45);
  EXPECT_EQ(absl::nullopt, session->unixSocketPeerCredentials());
  EXPECT_EQ(absl::nullopt, session->lastRoundTripTime());
  client_connection->close(Network::ConnectionCloseType::NoFlush);
}

TEST_P(QuicNetworkConnectionTest, Srtt) {
  initialize();

  Http::MockAlternateProtocolsCache rtt_cache;
  quic::QuicConfig config;
  PersistentQuicInfoImpl info{dispatcher_, 45};

  EXPECT_CALL(rtt_cache, getSrtt).WillOnce(Return(std::chrono::microseconds(5)));

  const int port = 30;
  std::unique_ptr<Network::ClientConnection> client_connection = createQuicNetworkConnection(
      info, crypto_config_,
      quic::QuicServerId{factory_->clientContextConfig().serverNameIndication(), port, false},
      dispatcher_, test_address_, test_address_, quic_stat_names_, rtt_cache, store_);

  EXPECT_EQ(info.quic_config_.GetInitialRoundTripTimeUsToSend(), 5);

  EnvoyQuicClientSession* session = static_cast<EnvoyQuicClientSession*>(client_connection.get());
  session->Initialize();
  client_connection->connect();
  EXPECT_TRUE(client_connection->connecting());
  client_connection->close(Network::ConnectionCloseType::NoFlush);
}

INSTANTIATE_TEST_SUITE_P(IpVersions, QuicNetworkConnectionTest,
                         testing::ValuesIn(TestEnvironment::getIpVersionsForTest()));
} // namespace Quic
} // namespace Envoy
