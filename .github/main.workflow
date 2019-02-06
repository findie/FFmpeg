workflow "New workflow" {
  on = "push"
  resolves = ["publish"]
}

action "Build" {
  uses = "actions/docker/cli@master"
  args = "build -t findie/ffmpeg ."
}

action "Login 2 DockerHub" {
  uses = "actions/docker/login@master"
  secrets = ["DOCKER_USERNAME", "DOCKER_PASSWORD"]
  needs = ["Build"]
}

action "publish" {
  uses = "actions/action-builder/docker@master"
  needs = ["Login 2 DockerHub"]
  runs = "make"
  args = "publish"
}
