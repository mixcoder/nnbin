#include "ebt/ebt.h"
#include "speech/speech.h"
#include "nn/lstm.h"
#include "nn/tensor-tree.h"
#include "nn/lstm-frame.h"
#include <fstream>

struct prediction_env {

    std::ifstream frame_batch;

    std::shared_ptr<tensor_tree::vertex> param;

    int layer;
    std::shared_ptr<tensor_tree::vertex> var_tree;

    std::vector<std::string> label;

    std::unordered_map<std::string, std::string> args;

    prediction_env(std::unordered_map<std::string, std::string> args);

    void run();

};

int main(int argc, char *argv[])
{
    ebt::ArgumentSpec spec {
        "frame-lstm-predict",
        "Predict frames with LSTM",
        {
            {"frame-batch", "", true},
            {"param", "", true},
            {"label", "", true},
            {"logprob", "", false},
        }
    };

    if (argc == 1) {
        ebt::usage(spec);
        exit(1);
    }

    auto args = ebt::parse_args(argc, argv, spec);

    for (int i = 0; i < argc; ++i) {
        std::cout << argv[i] << " ";
    }
    std::cout << std::endl;

    prediction_env env { args };

    env.run();

    return 0;
}

prediction_env::prediction_env(std::unordered_map<std::string, std::string> args)
    : args(args)
{
    frame_batch.open(args.at("frame-batch"));

    std::ifstream param_ifs { args.at("param") };
    std::string line;
    std::getline(param_ifs, line);
    layer = std::stoi(line);
    param = lstm_frame::make_tensor_tree(layer);
    tensor_tree::load_tensor(param, param_ifs);
    param_ifs.close();

    label = speech::load_label_set(args.at("label"));
}

void prediction_env::run()
{
    int nsample = 1;

    while (1) {
        std::vector<std::vector<double>> frames;

        frames = speech::load_frame_batch(frame_batch);

        if (!frame_batch) {
            break;
        }

        autodiff::computation_graph graph;
        std::vector<std::shared_ptr<autodiff::op_t>> inputs;

        for (int i = 0; i < frames.size(); ++i) {
            inputs.push_back(graph.var(la::vector<double>(frames[i])));
        }

        var_tree = tensor_tree::make_var_tree(graph, param);

        std::shared_ptr<lstm::lstm_step_transcriber> step
            = std::make_shared<lstm::dyer_lstm_step_transcriber>(lstm::dyer_lstm_step_transcriber{});

        std::shared_ptr<lstm::layered_transcriber> layered_trans
            = std::make_shared<lstm::layered_transcriber>(lstm::layered_transcriber {});

        for (int i = 0; i < layer; ++i) {
            layered_trans->layer.push_back(
                std::make_shared<lstm::bi_transcriber>(lstm::bi_transcriber{
                    std::make_shared<lstm::lstm_transcriber>(lstm::lstm_transcriber{step})
                }));
        }

        std::shared_ptr<lstm::transcriber> trans
            = std::make_shared<lstm::logsoftmax_transcriber>(
            lstm::logsoftmax_transcriber { layered_trans });

        std::vector<std::shared_ptr<autodiff::op_t>> logprob = (*trans)(var_tree, inputs);

        auto topo_order = autodiff::topo_order(logprob);
        autodiff::eval(topo_order, autodiff::eval_funcs);

        std::cout << nsample << ".phn" << std::endl;

        if (ebt::in(std::string("logprob"), args)) {
            for (int t = 0; t < logprob.size(); ++t) {
                auto& pred = autodiff::get_output<la::tensor_like<double>>(logprob[t]);

                std::cout << pred({0});

                for (int j = 1; j < pred.vec_size(); ++j) {
                    std::cout << " " << pred({j});
                }

                std::cout << std::endl;
            }
        } else {
            for (int t = 0; t < logprob.size(); ++t) {
                auto& pred = autodiff::get_output<la::tensor_like<double>>(logprob[t]);

                int argmax = -1;
                double max = -std::numeric_limits<double>::infinity();

                for (int j = 0; j < pred.vec_size(); ++j) {
                    if (pred({j}) > max) {
                        max = pred({j});
                        argmax = j;
                    }
                }

                std::cout << label[argmax] << std::endl;
            }
        }

        std::cout << "." << std::endl;

        ++nsample;
    }
}
